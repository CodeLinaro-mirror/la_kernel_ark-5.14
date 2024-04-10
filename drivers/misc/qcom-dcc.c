// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2015-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022-2024, Qualcomm Innovation Center, Inc. All rights reserved.
 */

/*
 * DCC(Data Capture and Compare) is a DMA engine designed for debugging
 * purposes.
 * In case of a system crash or manual software triggers by the user, the
 * DCC hardware stores the value at the register addresses which can be
 * used for debugging purposes.
 * The DCC driver provides the user with debugfs interface to configure the
 * register addresses. The options that the DCC hardware provides include
 * reading from registers and looping through the values of the same register.
 *
 * In certain cases the user might want to record the changing values of a
 * register with time for which he has the option to use the loop feature.
 *
 * The options mentioned above are exposed to the user by debugfs files.
 *
 * Usage:
 *  $ cd /sys/kernel/debug/qcom-dcc/../<linked list number>/config
 *  linked list number ranges from 0 to 7.
 *  Read type:
 *   $ echo R <address> [len] [apb|ahb] > config
 *   len is optional and defaults to value 1 (1 word or 4 bytes).
 *   apb|ahb represents APB bus type and AHB bus type respectively.
 *   Default value is ahb.
 *  Loop type:
 *   $ echo L <loop count> <number of addresses> <addr1>..<addr10> > config
 *   Maximum number of addresses is ten and max loop count varies between devices.
 *
 * Example:
 * Progam the linked list with addresses of Loop type.
 * $ echo L 0x8 0x2 0x10c004 0x10c00c > config
 * This reads the two addresses 0x10c004 and 0x10c00c for 8 times upon on a
 * SW/HW trigger.
 *
 * Program the linked list with the addresses of Read type.
 * $ echo R 0x10c004 > config
 * $ echo R 0x10c008 > config
 * $ echo R 0x10c00c > config
 * $ echo R 0x10c010 > config
 * ..... and so on for other timestamp related clk registers
 *
 * Other way of specifying is in "addr len" pair, in below case it
 * specifies to capture 4 words starting 0x10C004
 *
 * $ echo R 0x10C004 4 > config
 *
 * Configuration can be saved to a file and reuse it later.
 * $ cat config > /tmp/config_saved
 * Post reboot, write the file to config.
 * $ echo /tmp/config_saved > config
 *
 * Enable DCC
 * $ echo 1 > enable
 *
 * Send SW trigger
 * $ echo 1 > trigger
 *
 * Read SRAM
 * $ cat /dev/dcc_sram > dcc_sram.bin
 *
 * dcc_sram.bin can be converted to a human readable format with the parser tool.
 * Get the parser from
 * https://git.codelinaro.org/clo/le/platform/vendor/qcom-opensource/tools/-
 * /tree/opensource-tools.lnx.1.0.r176-rel/dcc_parser
 *
 * Parser tool usage:
 * python dcc_parser.py -s dcc_sram.bin --v2 -o output/
 *
 * Sample parsed output of dcc_sram.bin:
 *
 * <hwioDump version="1">
 *        <timestamp>03/14/21</timestamp>
 *            <generator>Linux DCC Parser</generator>
 *                <chip name="None" version="None">
 *                <register address="0x0010c004" value="0x80000000" />
 *                <register address="0x0010c008" value="0x00000008" />
 *                <register address="0x0010c00c" value="0x80004220" />
 *                <register address="0x0010c010" value="0x80000000" />
 *            </chip>
 *    <next_ll_offset>next_ll_offset : 0x1c </next_ll_offset> </hwioDump>
 *
 * Use --config-loopoffset with parser if loop type configuration is used.
 * To know the loop offset of your device, do
 * $ cat /sys/kernel/debug/qcom_dcc/../loop_offset
 *
 * More details and usage of this debugfs files are documented in
 * Documentation/ABI/testing/debugfs-driver-dcc.
 */

#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#define STATUS_READY_TIMEOUT		5000  /* microseconds */

/* DCC registers */
#define DCC_HW_INFO			0x04
#define DCC_LL_NUM_INFO			0x10
#define DCC_LL_LOCK			0x00
#define DCC_LL_CFG			0x04
#define DCC_LL_BASE			0x08
#define DCC_FD_BASE			0x0c
#define DCC_LL_OFFSET			0x80
#define DCC_LL_TIMEOUT			0x10
#define DCC_LL_INT_ENABLE		0x18
#define DCC_LL_INT_STATUS		0x1c
#define DCC_LL_SW_TRIGGER		0x2c
#define DCC_LL_BUS_ACCESS_STATUS	0x30

/* Default value used if a bit 6 in the HW_INFO register is set. */
#define DCC_FIX_LOOP_OFFSET		16

#define MAX_DCC_OFFSET			GENMASK(9, 2)
#define MAX_DCC_LEN			GENMASK(6, 0)
#define MAX_LOOP_CNT			GENMASK(7, 0)
#define MAX_LOOP_ADDR			10

#define DCC_ADDR_DESCRIPTOR		0x00
#define DCC_ADDR_LIMIT			27
#define DCC_WORD_SIZE			sizeof(u32)
#define DCC_ADDR_RANGE_MASK		GENMASK(31, 4)
#define DCC_LOOP_DESCRIPTOR		BIT(30)
#define DCC_LINK_DESCRIPTOR		GENMASK(31, 30)
#define DCC_STATUS_MASK			GENMASK(1, 0)
#define DCC_LOCK_MASK			BIT(0)
#define DCC_LOOP_OFFSET_MASK		BIT(6)
#define DCC_NUM_LLS_MASK		GENMASK(4, 0)

#define DCC_READ_IND			0x00

#define DCC_AHB_IND			0x00
#define DCC_APB_IND			BIT(29)

#define DCC_MAX_LINK_LIST		8

#define DCC_SRAM_WORD_LENGTH		4

#define LINE_BUFFER_MAX_SZ		50

enum dcc_mem_map_ver {
	MEM_MAP_VER2 = 2,
	MEM_MAP_VER3
};

enum dcc_descriptor_type {
	DCC_READ_TYPE,
	DCC_LOOP_TYPE,
};

/**
 * struct dcc_config_entry - configuration information related to each dcc instruction
 * @base:                    Base address of the register to be configured in dcc
 * @offset:                  Offset to the base address to be configured in dcc
 * @len:                     Length of the address in words of 4 bytes to be configured in dcc
 * @loop_cnt:                The number of times to loop on the register address in case
				of loop instructions
 * @apb_bus:                 Type of bus to be used for the instruction, can be either
				'apb' if 1 or 'ahb' if 0
 * @desc_type:               Stores the type of dcc instruction
 * @list:                    This is used to append this instruction to the list of
				instructions
 */
struct dcc_config_entry {
	u32				base;
	u32				offset;
	u32				len;
	u32				loop_cnt;
	bool				apb_bus;
	enum dcc_descriptor_type	desc_type;
	struct list_head		list;
};

/**
 * struct dcc_drvdata - configuration information related to a dcc device
 * @base:		Base Address of the dcc device
 * @dev:		The device attached to the driver data
 * @mutex:		Lock to protect access and manipulation of dcc_drvdata
 * @ram_base:		Base address for the SRAM dedicated for the dcc device
 * @ram_size:		Total size of the SRAM dedicated for the dcc device
 * @ram_offset:		Offset to the SRAM dedicated for dcc device
 * @ram_cfg:		Used for address limit calculation for dcc
 * @ram_start:		Starting address of DCC SRAM
 * @mem_map_ver:        Memory map version of DCC hardware
 * @sram_dev:		Miscellaneous device equivalent of dcc SRAM
 * @cfg_head:		Points to the head of the linked list of addresses
 * @dbg_dir:		The dcc debugfs directory under which all the debugfs files are placed
 * @max_link_list:	Total number of linkedlists supported by the DCC configuration
 * @loop_shift:		Loop offset bits range for the addresses
 * @enable_bitmap:	Bitmap to capture the enabled status of each linked list of addresses
 */
struct dcc_drvdata {
	void __iomem		*base;
	void __iomem            *ram_base;
	struct device		*dev;
	/* Lock to protect access and manipulation of dcc_drvdata */
	struct mutex		mutex;
	size_t			ram_size;
	u32			ram_offset;
	unsigned int		ram_cfg;
	unsigned int		ram_start;
	enum dcc_mem_map_ver	mem_map_ver;
	struct miscdevice	sram_dev;
	struct list_head	*cfg_head;
	struct dentry		*dbg_dir;
	size_t			max_link_list;
	u8			loop_shift;
	unsigned long		*enable_bitmap;
	char			**temp_buff_ptr;
};

struct dcc_cfg_attr {
	u32	addr;
	u32	prev_addr;
	u32	prev_off;
	u32	link;
	u32	sram_offset;
};

struct dcc_cfg_loop_attr {
	u32	loop_cnt;
	u32	loop_len;
	u32	loop_off;
	bool    loop_start;
};

static inline u32 dcc_list_offset(enum dcc_mem_map_ver version)
{
	if (version == MEM_MAP_VER2)
		return 0x2c;
	else
		return 0x34;
}

static inline void dcc_list_writel(struct dcc_drvdata *drvdata,
				   u32 val, u32 ll, u32 off)
{
	u32 offset = dcc_list_offset(drvdata->mem_map_ver) + off;

	writel(val, drvdata->base + ll * DCC_LL_OFFSET + offset);
}

static inline u32 dcc_list_readl(struct dcc_drvdata *drvdata, u32 ll, u32 off)
{
	u32 offset = dcc_list_offset(drvdata->mem_map_ver) + off;

	return readl(drvdata->base + ll * DCC_LL_OFFSET + offset);
}

static void dcc_sram_write_auto(struct dcc_drvdata *drvdata,
				u32 val, u32 *off)
{
	/* If the overflow condition is met increment the offset
	 * and return to indicate that overflow has occurred
	 */
	if (*off > drvdata->ram_size - 4) {
		*off += 4;
		return;
	}

	writel(val, drvdata->ram_base + *off);

	*off += 4;
}

static int dcc_sw_trigger(struct dcc_drvdata *drvdata)
{
	void __iomem *addr;
	int i;
	u32 status;
	u32 ll_cfg;
	u32 tmp_ll_cfg;
	u32 val;
	int ret = 0;

	mutex_lock(&drvdata->mutex);

	for (i = 0; i < drvdata->max_link_list; i++) {
		if (!test_bit(i, drvdata->enable_bitmap))
			continue;
		ll_cfg = dcc_list_readl(drvdata, i, DCC_LL_CFG);
		if (drvdata->mem_map_ver == MEM_MAP_VER3)
			tmp_ll_cfg = ll_cfg & ~BIT(8);
		else
			tmp_ll_cfg = ll_cfg & ~BIT(9);
		dcc_list_writel(drvdata, tmp_ll_cfg, i, DCC_LL_CFG);
		dcc_list_writel(drvdata, 1, i, DCC_LL_SW_TRIGGER);
		dcc_list_writel(drvdata, ll_cfg, i, DCC_LL_CFG);
	}

	addr = drvdata->base + DCC_LL_INT_STATUS;
	if (readl_poll_timeout(addr, val, !FIELD_GET(DCC_STATUS_MASK, val),
			       1, STATUS_READY_TIMEOUT)) {
		dev_err(drvdata->dev, "DCC is busy after receiving sw trigger\n");
		ret = -EBUSY;
		goto out_unlock;
	}

	for (i = 0; i < drvdata->max_link_list; i++) {
		if (!test_bit(i, drvdata->enable_bitmap))
			continue;

		status = dcc_list_readl(drvdata, i, DCC_LL_BUS_ACCESS_STATUS);
		if (!status)
			continue;

		dev_err(drvdata->dev, "Read access error for list %d err: 0x%x\n",
			i, status);
		ll_cfg = dcc_list_readl(drvdata, i, DCC_LL_CFG);
		if (drvdata->mem_map_ver == MEM_MAP_VER3)
			tmp_ll_cfg = ll_cfg & ~BIT(8);
		else
			tmp_ll_cfg = ll_cfg & ~BIT(9);
		dcc_list_writel(drvdata, tmp_ll_cfg, i, DCC_LL_CFG);
		dcc_list_writel(drvdata, DCC_STATUS_MASK, i, DCC_LL_BUS_ACCESS_STATUS);
		dcc_list_writel(drvdata, ll_cfg, i, DCC_LL_CFG);
		ret = -ENODATA;
		break;
	}

out_unlock:
	mutex_unlock(&drvdata->mutex);
	return ret;
}

static void dcc_ll_cfg_reset_link(struct dcc_cfg_attr *cfg)
{
	cfg->addr = 0x00;
	cfg->link = 0;
	cfg->prev_off = 0;
	cfg->prev_addr = cfg->addr;
}

static void dcc_emit_loop(struct dcc_drvdata *drvdata, struct dcc_config_entry *entry,
			  struct dcc_cfg_attr *cfg,
			  struct dcc_cfg_loop_attr *cfg_loop,
			  u32 *total_len)
{
	int loop;

	/* Check if we need to write link of prev entry */
	if (cfg->link)
		dcc_sram_write_auto(drvdata, cfg->link, &cfg->sram_offset);

	if (cfg_loop->loop_start) {
		loop = (cfg->sram_offset - cfg_loop->loop_off) / 4;
		loop |= (cfg_loop->loop_cnt << drvdata->loop_shift) &
				   GENMASK(DCC_ADDR_LIMIT, drvdata->loop_shift);
		loop |= DCC_LOOP_DESCRIPTOR;
		*total_len += (*total_len - cfg_loop->loop_len) * cfg_loop->loop_cnt;

		dcc_sram_write_auto(drvdata, loop, &cfg->sram_offset);

		cfg_loop->loop_start = false;
		cfg_loop->loop_len = 0;
		cfg_loop->loop_off = 0;
	} else {
		cfg_loop->loop_start = true;
		cfg_loop->loop_cnt = entry->loop_cnt - 1;
		cfg_loop->loop_len = *total_len;
		cfg_loop->loop_off = cfg->sram_offset;
	}

	/* Reset link and prev_off */
	dcc_ll_cfg_reset_link(cfg);
}

static int dcc_emit_read(struct dcc_drvdata *drvdata,
			 struct dcc_config_entry *entry,
			 struct dcc_cfg_attr *cfg,
			 u32 *pos, u32 *total_len)
{
	u32 off;
	u32 temp_off;

	cfg->addr = (entry->base >> 4) & GENMASK(27, 0);

	if (entry->apb_bus)
		cfg->addr |= DCC_ADDR_DESCRIPTOR | DCC_READ_IND | DCC_APB_IND;
	else
		cfg->addr |= DCC_ADDR_DESCRIPTOR | DCC_READ_IND | DCC_AHB_IND;

	off = entry->offset / 4;

	*total_len += entry->len * 4;

	if (!cfg->prev_addr || cfg->prev_addr != cfg->addr || cfg->prev_off > off) {
		/* Check if we need to write prev link entry */
		if (cfg->link)
			dcc_sram_write_auto(drvdata, cfg->link, &cfg->sram_offset);
		dev_dbg(drvdata->dev, "DCC: sram address 0x%x\n", cfg->sram_offset);

		/* Write address */
		dcc_sram_write_auto(drvdata, cfg->addr, &cfg->sram_offset);

		/* Reset link and prev_off */
		cfg->link = 0;
		cfg->prev_off = 0;
	}

	if ((off - cfg->prev_off) > 0xff || entry->len > MAX_DCC_LEN) {
		dev_err(drvdata->dev, "DCC: Programming error Base: 0x%x, offset 0x%x\n",
			entry->base, entry->offset);
		return -EINVAL;
	}

	if (cfg->link) {
		/*
		 * link already has one offset-length so new
		 * offset-length needs to be placed at
		 * bits [29:15]
		 */
		*pos = 15;

		/* Clear bits [31:16] */
		cfg->link &= GENMASK(14, 0);
	} else {
		/*
		 * link is empty, so new offset-length needs
		 * to be placed at bits [15:0]
		 */
		*pos = 0;
		cfg->link = 1 << 15;
	}

	/* write new offset-length pair to correct position */
	temp_off = (off - cfg->prev_off) & GENMASK(7, 0);
	cfg->link |= (temp_off | ((entry->len << 8) & GENMASK(14, 8))) << *pos;

	cfg->link |= DCC_LINK_DESCRIPTOR;

	if (*pos) {
		dcc_sram_write_auto(drvdata, cfg->link, &cfg->sram_offset);
		cfg->link = 0;
	}

	cfg->prev_off  = off + entry->len - 1;
	cfg->prev_addr = cfg->addr;
	return 0;
}

static int dcc_emit_config(struct dcc_drvdata *drvdata, unsigned int curr_list)
{
	int ret;
	u32 total_len, pos;
	struct dcc_config_entry *entry;
	struct dcc_cfg_attr cfg = {0};
	struct dcc_cfg_loop_attr cfg_loop = {0};

	cfg.sram_offset = drvdata->ram_cfg * 4;
	total_len = 0;

	list_for_each_entry(entry, &drvdata->cfg_head[curr_list], list) {
		switch (entry->desc_type) {
		case DCC_LOOP_TYPE:
			dcc_emit_loop(drvdata, entry, &cfg, &cfg_loop, &total_len);
			break;

		case DCC_READ_TYPE:
			ret = dcc_emit_read(drvdata, entry, &cfg, &pos, &total_len);
			if (ret)
				goto err;
			break;
		}
	}

	if (cfg.link)
		dcc_sram_write_auto(drvdata, cfg.link, &cfg.sram_offset);

	if (cfg_loop.loop_start) {
		dev_err(drvdata->dev, "DCC: Programming error: Loop unterminated\n");
		ret = -EINVAL;
		goto err;
	}

	/* Setting zero to indicate end of the list */
	cfg.link = DCC_LINK_DESCRIPTOR;
	dcc_sram_write_auto(drvdata, cfg.link, &cfg.sram_offset);

	/* Check if sram offset exceeds the ram size */
	if (cfg.sram_offset > drvdata->ram_size)
		goto overstep;

	/* Update ram_cfg and check if the data will overstep */
	drvdata->ram_cfg = (cfg.sram_offset + total_len) / 4;

	if (cfg.sram_offset + total_len > drvdata->ram_size) {
		cfg.sram_offset += total_len;
		goto overstep;
	}

	drvdata->ram_start = cfg.sram_offset / 4;
	return 0;
overstep:
	ret = -E2BIG;
	memset_io(drvdata->ram_base, 0, drvdata->ram_size);

err:
	return ret;
}

static bool dcc_valid_list(struct dcc_drvdata *drvdata, unsigned int curr_list)
{
	u32 lock_reg;

	if (list_empty(&drvdata->cfg_head[curr_list]))
		return false;

	if (test_bit(curr_list, drvdata->enable_bitmap)) {
		dev_err(drvdata->dev, "List %d is already enabled\n", curr_list);
		return false;
	}

	lock_reg = dcc_list_readl(drvdata, curr_list, DCC_LL_LOCK);
	if (lock_reg & DCC_LOCK_MASK) {
		dev_err(drvdata->dev, "List %d is already locked\n", curr_list);
		return false;
	}

	return true;
}

static bool is_dcc_enabled(struct dcc_drvdata *drvdata)
{
	int list;

	for (list = 0; list < drvdata->max_link_list; list++)
		if (test_bit(list, drvdata->enable_bitmap))
			return true;

	return false;
}

static int dcc_enable(struct dcc_drvdata *drvdata, unsigned int curr_list)
{
	int ret;
	u32 ram_cfg_base;

	mutex_lock(&drvdata->mutex);

	if (!dcc_valid_list(drvdata, curr_list)) {
		ret = -EINVAL;
		goto out_unlock;
	}

	/* Fill dcc sram with the poison value.
	 * This helps in understanding bus
	 * hang from registers returning a zero
	 */
	if (!is_dcc_enabled(drvdata))
		memset_io(drvdata->ram_base, 0xde, drvdata->ram_size);

	/* 1. Take ownership of the list */
	dcc_list_writel(drvdata, DCC_LOCK_MASK, curr_list, DCC_LL_LOCK);

	/* 2. Program linked-list in the SRAM */
	ram_cfg_base = drvdata->ram_cfg;
	ret = dcc_emit_config(drvdata, curr_list);
	if (ret) {
		dcc_list_writel(drvdata, 0, curr_list, DCC_LL_LOCK);
		goto out_unlock;
	}

	/* 3. Program DCC_RAM_CFG reg */
	dcc_list_writel(drvdata, ram_cfg_base +
			drvdata->ram_offset / 4, curr_list, DCC_LL_BASE);
	dcc_list_writel(drvdata, drvdata->ram_start +
			drvdata->ram_offset / 4, curr_list, DCC_FD_BASE);
	dcc_list_writel(drvdata, 0xFFF, curr_list, DCC_LL_TIMEOUT);

	/* 4. Clears interrupt status register */
	dcc_list_writel(drvdata, 0, curr_list, DCC_LL_INT_ENABLE);
	dcc_list_writel(drvdata, (BIT(0) | BIT(1) | BIT(2)),
			curr_list, DCC_LL_INT_STATUS);

	set_bit(curr_list, drvdata->enable_bitmap);

	/* 5. Configure trigger */
	if (drvdata->mem_map_ver == MEM_MAP_VER3)
		dcc_list_writel(drvdata, BIT(8), curr_list, DCC_LL_CFG);
	else
		dcc_list_writel(drvdata, BIT(9), curr_list, DCC_LL_CFG);

out_unlock:
	mutex_unlock(&drvdata->mutex);
	return ret;
}

static void dcc_disable(struct dcc_drvdata *drvdata, int curr_list)
{
	mutex_lock(&drvdata->mutex);

	if (!test_bit(curr_list, drvdata->enable_bitmap))
		goto out_unlock;
	dcc_list_writel(drvdata, 0, curr_list, DCC_LL_CFG);
	dcc_list_writel(drvdata, 0, curr_list, DCC_LL_BASE);
	dcc_list_writel(drvdata, 0, curr_list, DCC_FD_BASE);
	dcc_list_writel(drvdata, 0, curr_list, DCC_LL_LOCK);
	clear_bit(curr_list, drvdata->enable_bitmap);
out_unlock:
	mutex_unlock(&drvdata->mutex);
}

static u32 dcc_filp_curr_list(const struct file *filp)
{
	struct dentry *dentry = file_dentry(filp);
	int curr_list, ret;

	ret = kstrtoint(dentry->d_parent->d_name.name, 0, &curr_list);
	if (ret)
		return ret;

	return curr_list;
}

static ssize_t enable_read(struct file *filp, char __user *userbuf,
			   size_t count, loff_t *ppos)
{
	char *buf;
	int curr_list = dcc_filp_curr_list(filp);
	struct dcc_drvdata *drvdata = filp->private_data;

	if (curr_list < 0)
		return curr_list;

	mutex_lock(&drvdata->mutex);
	if (test_bit(curr_list, drvdata->enable_bitmap))
		buf = "Y\n";
	else
		buf = "N\n";
	mutex_unlock(&drvdata->mutex);

	return simple_read_from_buffer(userbuf, count, ppos, buf, strlen(buf));
}

static ssize_t enable_write(struct file *filp, const char __user *userbuf,
			    size_t count, loff_t *ppos)
{
	int ret = 0, curr_list;
	bool val;
	struct dcc_drvdata *drvdata = filp->private_data;

	curr_list = dcc_filp_curr_list(filp);
	if (curr_list < 0)
		return curr_list;

	ret = kstrtobool_from_user(userbuf, count, &val);
	if (ret < 0)
		return ret;

	if (val) {
		ret = dcc_enable(drvdata, curr_list);
		if (ret)
			return ret;
	} else {
		dcc_disable(drvdata, curr_list);
	}

	return count;
}

static const struct file_operations enable_fops = {
	.read = enable_read,
	.write = enable_write,
	.open = simple_open,
	.llseek = generic_file_llseek,
};

static ssize_t trigger_write(struct file *filp,
			     const char __user *user_buf, size_t count,
			     loff_t *ppos)
{
	int ret;
	unsigned int val;
	struct dcc_drvdata *drvdata = filp->private_data;

	ret = kstrtouint_from_user(user_buf, count, 0, &val);
	if (ret < 0)
		return ret;

	if (val != 1)
		return -EINVAL;

	ret = dcc_sw_trigger(drvdata);
	if (ret < 0)
		return ret;

	return count;
}

static const struct file_operations trigger_fops = {
	.write = trigger_write,
	.open = simple_open,
	.llseek = generic_file_llseek,
};

static int dcc_config_add(struct dcc_drvdata *drvdata, unsigned int addr,
			  unsigned int len, bool apb_bus, int curr_list)
{
	int ret = 0;
	struct dcc_config_entry *entry, *pentry;
	unsigned int base, offset;

	mutex_lock(&drvdata->mutex);

	if (!len || len > drvdata->ram_size / DCC_WORD_SIZE) {
		dev_err(drvdata->dev, "DCC: Invalid length\n");
		ret = -EINVAL;
		goto out_unlock;
	}

	base = addr & DCC_ADDR_RANGE_MASK;

	if (!list_empty(&drvdata->cfg_head[curr_list])) {
		pentry = list_last_entry(&drvdata->cfg_head[curr_list],
					 struct dcc_config_entry, list);

		if (pentry->desc_type == DCC_READ_TYPE &&
		    addr >= (pentry->base + pentry->offset) &&
		    addr <= (pentry->base + pentry->offset + MAX_DCC_OFFSET)) {
			/* Re-use base address from last entry */
			base = pentry->base;

			if ((pentry->len * 4 + pentry->base + pentry->offset)
					== addr) {
				len += pentry->len;

				if (len > MAX_DCC_LEN)
					pentry->len = MAX_DCC_LEN;
				else
					pentry->len = len;

				addr = pentry->base + pentry->offset +
					pentry->len * 4;
				len -= pentry->len;
			}
		}
	}

	offset = addr - base;

	while (len) {
		entry = kzalloc(sizeof(*entry), GFP_KERNEL);
		if (!entry) {
			ret = -ENOMEM;
			goto out_unlock;
		}

		entry->base = base;
		entry->offset = offset;
		entry->len = min_t(u32, len, MAX_DCC_LEN);
		entry->desc_type = DCC_READ_TYPE;
		entry->apb_bus = apb_bus;
		INIT_LIST_HEAD(&entry->list);
		list_add_tail(&entry->list,
			      &drvdata->cfg_head[curr_list]);

		len -= entry->len;
		offset += MAX_DCC_LEN * 4;
	}

out_unlock:
	mutex_unlock(&drvdata->mutex);
	return ret;
}

static ssize_t dcc_config_add_read(struct dcc_drvdata *drvdata, char *buf, int curr_list)
{
	bool bus;
	int len, nval;
	unsigned int base;
	char apb_bus[4];

	nval = sscanf(buf, "%x %i %3s", &base, &len, apb_bus);
	if (nval <= 0 || nval > 3)
		return -EINVAL;

	if (nval == 1) {
		len = 1;
		bus = false;
	} else if (nval == 2) {
		bus = false;
	} else if (!strcmp("apb", apb_bus)) {
		bus = true;
	} else if (!strcmp("ahb", apb_bus)) {
		bus = false;
	} else {
		return -EINVAL;
	}

	return dcc_config_add(drvdata, base, len, bus, curr_list);
}

static void dcc_config_reset(struct dcc_drvdata *drvdata)
{
	struct dcc_config_entry *entry, *temp;
	int curr_list;

	mutex_lock(&drvdata->mutex);

	for (curr_list = 0; curr_list < drvdata->max_link_list; curr_list++) {
		list_for_each_entry_safe(entry, temp,
					 &drvdata->cfg_head[curr_list], list) {
			list_del(&entry->list);
			kfree(entry);
		}
	}
	drvdata->ram_start = 0;
	drvdata->ram_cfg = 0;
	mutex_unlock(&drvdata->mutex);
}

static ssize_t config_reset_write(struct file *filp,
				  const char __user *user_buf, size_t count,
				  loff_t *ppos)
{
	unsigned int val, ret;
	struct dcc_drvdata *drvdata = filp->private_data;

	ret = kstrtouint_from_user(user_buf, count, 0, &val);
	if (ret < 0)
		return ret;

	if (val)
		dcc_config_reset(drvdata);

	return count;
}

static const struct file_operations config_reset_fops = {
	.write = config_reset_write,
	.open = simple_open,
	.llseek = generic_file_llseek,
};

static ssize_t ready_read(struct file *filp, char __user *userbuf,
			  size_t count, loff_t *ppos)
{
	char *buf;
	struct dcc_drvdata *drvdata = filp->private_data;

	if (!is_dcc_enabled(drvdata))
		return -EINVAL;

	if (!FIELD_GET(BIT(1), readl(drvdata->base + DCC_LL_INT_STATUS)))
		buf = "Y\n";
	else
		buf = "N\n";

	return simple_read_from_buffer(userbuf, count, ppos, buf, strlen(buf) + 1);
}

static const struct file_operations ready_fops = {
	.read = ready_read,
	.open = simple_open,
	.llseek = generic_file_llseek,
};

static ssize_t loop_offset_read(struct file *filp, char __user *userbuf,
				size_t count, loff_t *ppos)
{
	char buf[10];
	struct dcc_drvdata *drvdata = filp->private_data;

	scnprintf(buf, sizeof(buf), "%d\n", drvdata->loop_shift);
	return simple_read_from_buffer(userbuf, count, ppos, buf, strlen(buf) + 1);
}

static const struct file_operations loop_offset_fops = {
	.read = loop_offset_read,
	.open = simple_open,
	.llseek = generic_file_llseek,
};

static int dcc_add_loop(struct dcc_drvdata *drvdata, unsigned long loop_cnt, int curr_list)
{
	struct dcc_config_entry *entry;

	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry)
		return -ENOMEM;

	entry->loop_cnt = min_t(u32, loop_cnt, MAX_LOOP_CNT);
	entry->desc_type = DCC_LOOP_TYPE;
	INIT_LIST_HEAD(&entry->list);
	list_add_tail(&entry->list, &drvdata->cfg_head[curr_list]);

	return 0;
}

static ssize_t dcc_config_add_loop(struct dcc_drvdata *drvdata, char *buf, int curr_list)
{
	int ret, i = 0;
	char *token, *input;
	char delim[2] = " ";
	unsigned int val[MAX_LOOP_ADDR];

	input = buf;

	while ((token = strsep(&input, delim)) && i < MAX_LOOP_ADDR) {
		ret = kstrtouint(token, 0, &val[i++]);
		if (ret)
			return ret;
	}

	if (token) {
		dev_err(drvdata->dev, "Max limit %u of loop address exceeded",
			MAX_LOOP_ADDR);
		return -EINVAL;
	}

	if (val[1] < 1 || val[1] > 8 || val[1] > (i - 2))
		return -EINVAL;

	ret = dcc_add_loop(drvdata, val[0], curr_list);
	if (ret)
		return ret;

	for (i = 0; i < val[1]; i++)
		dcc_config_add(drvdata, val[i + 2], 1, false, curr_list);

	return dcc_add_loop(drvdata, 1, curr_list);
}

static int config_show(struct seq_file *m, void *data)
{
	struct dcc_drvdata *drvdata = m->private;
	struct dcc_config_entry *entry, *loop_entry;
	int index = 0, curr_list, i;
	unsigned int loop_val[MAX_LOOP_ADDR];

	curr_list = dcc_filp_curr_list(m->file);
	if (curr_list < 0)
		return curr_list;

	mutex_lock(&drvdata->mutex);

	list_for_each_entry(entry, &drvdata->cfg_head[curr_list], list) {
		index++;
		switch (entry->desc_type) {
		case DCC_LOOP_TYPE:
			loop_entry = entry;
			loop_val[0] = loop_entry->loop_cnt;
			loop_entry = list_next_entry(loop_entry, list);
			for (i = 0; i < (MAX_LOOP_ADDR - 2);
					i++, loop_entry = list_next_entry(loop_entry, list)) {
				if (loop_entry->desc_type == DCC_READ_TYPE) {
					loop_val[i + 2] = loop_entry->base + loop_entry->offset;
				} else if (loop_entry->desc_type == DCC_LOOP_TYPE) {
					loop_val[i + 2] = loop_entry->loop_cnt;
					loop_val[1] = i;
					entry = loop_entry;
					break;
				}
			}
			seq_printf(m, "L 0x%x 0x%x", loop_val[0], loop_val[1]);
			for (i = 0; i < loop_val[1]; i++)
				seq_printf(m, " 0x%x", loop_val[i + 2]);
			seq_puts(m, "\n");
			break;
		case DCC_READ_TYPE:
			seq_printf(m, "R 0x%x 0x%x %s\n",
				   entry->base + entry->offset,
				   entry->len,
				   entry->apb_bus ? "apb" : "ahb");
		}
	}
	mutex_unlock(&drvdata->mutex);
	return 0;
}

static int config_open(struct inode *inode, struct file *file)
{
	struct dcc_drvdata *drvdata = inode->i_private;

	return single_open(file, config_show, drvdata);
}

static ssize_t config_write(struct file *filp,
			    const char __user *user_buf, size_t count,
			    loff_t *ppos)
{
	int ret, curr_list;
	char *token, *line;
	char *buf, *bufp, *temp_buff;
	char *delim = " ";
	struct dcc_drvdata *drvdata = filp->f_inode->i_private;
	ssize_t processed_len = 0;

	if (count == 0)
		return -EINVAL;
	buf = kzalloc(count, GFP_KERNEL);
	if (buf)
		bufp = buf;
	else
		return -ENOMEM;

	ret = copy_from_user(buf, user_buf, count);
	if (ret)
		goto err;

	curr_list = dcc_filp_curr_list(filp);
	if (curr_list < 0) {
		ret = curr_list;
		goto err;
	}

	while (bufp[0] != '\0') {
		/* Parse line by line */
		line = strsep(&bufp, "\n");
		if (line && bufp) {
			processed_len += strlen(line) + 1;
			if (drvdata->temp_buff_ptr && drvdata->temp_buff_ptr[curr_list]) {
				temp_buff = drvdata->temp_buff_ptr[curr_list];
				/* Size of combined string must not be greater than
				 * allowed line size.
				 */
				if (strlen(line) + strlen(temp_buff) + 1 > LINE_BUFFER_MAX_SZ) {
					dev_err(drvdata->dev, "Invalid input\n");
					return -EINVAL;
				}
				strlcat(temp_buff, line, strlen(line) + 1);
				line = temp_buff;
				kfree(temp_buff);
				drvdata->temp_buff_ptr[curr_list] = NULL;
			}

			token = strsep(&line, delim);

			if (!strcmp("R", token)) {
				ret = dcc_config_add_read(drvdata, line, curr_list);
			} else if (!strcmp("L", token)) {
				ret = dcc_config_add_loop(drvdata, line, curr_list);
			} else {
				dev_err(drvdata->dev, "%s is not a correct input\n", token);
				return -EINVAL;
			}

			if (ret)
				goto err;
		} else {
			/* Save the incomplete line to a temporary buffer and rejoin it later */
			if (!drvdata->temp_buff_ptr) {
				drvdata->temp_buff_ptr = devm_kcalloc(drvdata->dev,
								      drvdata->max_link_list,
								      sizeof(char *),
								      GFP_KERNEL);
				if (!drvdata->temp_buff_ptr) {
					ret = -ENOMEM;
					goto err;
				}
			}
			drvdata->temp_buff_ptr[curr_list] = kzalloc(LINE_BUFFER_MAX_SZ,
								    GFP_KERNEL);
			temp_buff = drvdata->temp_buff_ptr[curr_list];
			if (!temp_buff) {
				ret = -ENOMEM;
				goto err;
			}
			if ((count - processed_len) >= LINE_BUFFER_MAX_SZ) {
				dev_err(drvdata->dev, "Invalid input\n");
				ret = -EINVAL;
				goto err;
			}
			memcpy(temp_buff, line, count - processed_len);
			temp_buff[count - processed_len + 1] = '\0';
			processed_len += (strlen(temp_buff) + 1);
			break;
		}
	}

	kfree(buf);
	return processed_len;

err:
	kfree(buf);
	if (drvdata->temp_buff_ptr && drvdata->temp_buff_ptr[curr_list]) {
		kfree(drvdata->temp_buff_ptr[curr_list]);
		drvdata->temp_buff_ptr[curr_list] = NULL;
	}
	return ret;
}

static const struct file_operations config_fops = {
	.open = config_open,
	.read = seq_read,
	.write = config_write,
	.llseek = seq_lseek,
	.release = single_release,
};

static void dcc_delete_debug_dir(struct dcc_drvdata *drvdata)
{
	 debugfs_remove_recursive(drvdata->dbg_dir);
};

static void dcc_create_debug_dir(struct dcc_drvdata *drvdata)
{
	int i;
	char list_num[10];
	struct dentry *dcc_dev, *list;
	struct device *dev = drvdata->dev;

	drvdata->dbg_dir = debugfs_create_dir(KBUILD_MODNAME, NULL);
	dcc_dev = debugfs_create_dir(dev_name(dev), drvdata->dbg_dir);

	for (i = 0; i <= drvdata->max_link_list; i++) {
		scnprintf(list_num, sizeof(list_num), "%d", i);
		list = debugfs_create_dir(list_num, dcc_dev);
		debugfs_create_file("enable", 0600, list, drvdata, &enable_fops);
		debugfs_create_file("config", 0600, list, drvdata, &config_fops);
	}

	debugfs_create_file("trigger", 0200, drvdata->dbg_dir, drvdata, &trigger_fops);
	debugfs_create_file("ready", 0400, drvdata->dbg_dir, drvdata, &ready_fops);
	debugfs_create_file("config_reset", 0200, drvdata->dbg_dir, drvdata, &config_reset_fops);
	debugfs_create_file("loop_offset", 0400, drvdata->dbg_dir, drvdata, &loop_offset_fops);
}

static ssize_t dcc_sram_read(struct file *file, char __user *data,
			     size_t len, loff_t *ppos)
{
	unsigned char *buf;
	struct dcc_drvdata *drvdata;

	drvdata = container_of(file->private_data, struct dcc_drvdata,
			       sram_dev);

	/* EOF check */
	if (*ppos >= drvdata->ram_size)
		return 0;

	if ((*ppos + len) > drvdata->ram_size)
		len = (drvdata->ram_size - *ppos);

	buf = kzalloc(len, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	memcpy_fromio(buf, drvdata->ram_base + *ppos, len);

	if (copy_to_user(data, buf, len)) {
		kfree(buf);
		return -EFAULT;
	}

	*ppos += len;

	kfree(buf);

	return len;
}

static const struct file_operations dcc_sram_fops = {
	.owner		= THIS_MODULE,
	.read		= dcc_sram_read,
	.llseek		= no_llseek,
};

static int dcc_sram_dev_init(struct dcc_drvdata *drvdata)
{
	drvdata->sram_dev.minor = MISC_DYNAMIC_MINOR;
	drvdata->sram_dev.name = "dcc_sram";
	drvdata->sram_dev.fops = &dcc_sram_fops;

	return misc_register(&drvdata->sram_dev);
}

static void dcc_sram_dev_exit(struct dcc_drvdata *drvdata)
{
	misc_deregister(&drvdata->sram_dev);
}

static int dcc_probe(struct platform_device *pdev)
{
	u32 val;
	int ret = 0, i;
	struct device *dev = &pdev->dev;
	struct dcc_drvdata *drvdata;
	struct resource *res;

	drvdata = devm_kzalloc(dev, sizeof(*drvdata), GFP_KERNEL);
	if (!drvdata)
		return -ENOMEM;

	drvdata->dev = &pdev->dev;
	platform_set_drvdata(pdev, drvdata);

	drvdata->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(drvdata->base))
		return PTR_ERR(drvdata->base);

	drvdata->ram_base = devm_platform_get_and_ioremap_resource(pdev, 1, &res);
	if (IS_ERR(drvdata->ram_base))
		return PTR_ERR(drvdata->ram_base);

	drvdata->ram_size = resource_size(res);
	ret = of_property_read_u32(pdev->dev.of_node, "qcom,dcc-offset",
				   &drvdata->ram_offset);
	if (ret)
		return -EINVAL;

	drvdata->mem_map_ver = (enum dcc_mem_map_ver)of_device_get_match_data(&pdev->dev);

	if (drvdata->mem_map_ver != MEM_MAP_VER3 && drvdata->mem_map_ver != MEM_MAP_VER2) {
		dev_err(drvdata->dev, "Unsupported memory map version 0x%x\n",
			drvdata->mem_map_ver);
		return -EINVAL;
	}

	drvdata->max_link_list = readl(drvdata->base + DCC_LL_NUM_INFO) & DCC_NUM_LLS_MASK;
	if (!drvdata->max_link_list)
		return  -EINVAL;

	val = readl(drvdata->base + DCC_HW_INFO);
	/* Either set the fixed loop offset or calculate
	 * it from the total number of words in dcc_sram.
	 * Max consecutive addresses dcc can loop is
	 * equivalent to the words in dcc_sram.
	 */
	if (val & DCC_LOOP_OFFSET_MASK)
		drvdata->loop_shift = DCC_FIX_LOOP_OFFSET;
	else
		drvdata->loop_shift = get_bitmask_order((drvdata->ram_offset +
					drvdata->ram_size) / DCC_SRAM_WORD_LENGTH - 1);

	mutex_init(&drvdata->mutex);

	drvdata->enable_bitmap = devm_kcalloc(dev, BITS_TO_LONGS(drvdata->max_link_list),
					      sizeof(*drvdata->enable_bitmap), GFP_KERNEL);
	if (!drvdata->enable_bitmap)
		return -ENOMEM;

	drvdata->cfg_head = devm_kcalloc(dev, drvdata->max_link_list,
					 sizeof(*drvdata->cfg_head), GFP_KERNEL);
	if (!drvdata->cfg_head)
		return -ENOMEM;

	for (i = 0; i < drvdata->max_link_list; i++)
		INIT_LIST_HEAD(&drvdata->cfg_head[i]);

	ret = dcc_sram_dev_init(drvdata);
	if (ret) {
		dev_err(drvdata->dev, "DCC: sram node not registered.\n");
		return ret;
	}

	dcc_create_debug_dir(drvdata);

	return 0;
}

static int dcc_remove(struct platform_device *pdev)
{
	struct dcc_drvdata *drvdata = platform_get_drvdata(pdev);

	dcc_delete_debug_dir(drvdata);
	dcc_sram_dev_exit(drvdata);
	dcc_config_reset(drvdata);

	return 0;
}

static const struct of_device_id dcc_match_table[] = {
	{ .compatible = "qcom,dcc-v2", .data = (void *)MEM_MAP_VER2 },
	{ .compatible = "qcom,dcc-v3", .data = (void *)MEM_MAP_VER3 },
	{ }
};
MODULE_DEVICE_TABLE(of, dcc_match_table);

static struct platform_driver dcc_driver = {
	.probe = dcc_probe,
	.remove	= dcc_remove,
	.driver	= {
		.name = "qcom-dcc",
		.of_match_table	= dcc_match_table,
	},
};

module_platform_driver(dcc_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Qualcomm Technologies Inc. DCC driver");
