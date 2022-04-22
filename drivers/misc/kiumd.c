/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (c) 2022 Qualcomm Innovation Center, Inc. All rights reserved.
 */
#include <linux/completion.h>
#include <linux/device.h>
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/idr.h>
#include <linux/list.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of.h>
#include <linux/sort.h>
#include <linux/of_platform.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/vfio.h>
#include <linux/hashtable.h>
#include <uapi/misc/kiumd.h>

/*Global Data structures needed for buffer sharing */
static DEFINE_MUTEX(g_kiumd_lock);
//static DEFINE_HASHTABLE(g_dmabuf_kiumd_table, 10);

struct dmabuf_fd {
	struct dma_buf *kiumd_dmabuf; //Value
	uint32_t token;  //Key
	struct list_head *next;
};
struct dmabuf_fd dmabuf_tbl[1024];
static uint32_t g_counter = 0;

struct kiumd_dev {
	struct device* dev;
	struct miscdevice miscdev;
	int fd;
};

int kiumd_dmabuf_vfio_map(struct kiumd_dev *ki_dev, char __user *arg)
{
	struct kiumd_user kiusr;
	struct vfio_device *vfio_dev;
	struct file *file;
	struct dma_buf *kiumd_dmabuf = NULL;
	struct dma_buf_attachment *dmabufattach = NULL;
	struct sg_table *sgt = NULL;

	if (copy_from_user(&kiusr, arg, sizeof(struct kiumd_user)))
		return -EFAULT;


	file = fget(kiusr.vfio_fd);
	vfio_dev = (struct vfio_device *)file->private_data;
	if(vfio_dev == NULL) {
		pr_err("%s:vfio_dev is NULL \n",__func__);
		return -ENOTTY;
	}
	// struct device st
	//dma_buf_get (fd ) -> dma_buf*
	kiumd_dmabuf = dma_buf_get(kiusr.dma_buf_fd);
	if(kiumd_dmabuf == NULL) {
		pr_err("%s:kiumd_dmabuf is NULL \n",__func__);
		return -ENOTTY;
	}
	//dma_buf_attach
	if (vfio_dev->dev != NULL)
		dmabufattach = dma_buf_attach(kiumd_dmabuf, vfio_dev->dev);
	if(dmabufattach == NULL) {
		pr_err("%s:dmabufattach is NULL \n",__func__);
		return -ENOTTY;
	}

	//dma_buf_map_attachment
	sgt = dma_buf_map_attachment(dmabufattach, DMA_BIDIRECTIONAL);
	if(sgt == NULL) {
		pr_err("%s:sgt is NULL \n",__func__);
		return -ENOTTY;
	}

	kiusr.sgt_ptr = sgt;
	kiusr.dmabufattach = dmabufattach;
	kiusr.dma_addr = sg_dma_address(sgt->sgl);
	kiusr.dmabuf_ptr = kiumd_dmabuf;
	copy_to_user(arg, &kiusr, sizeof(kiusr));

	return 0;
}

int kiumd_dmabuf_vfio_unmap(struct kiumd_dev *ki_dev, char __user *arg)
{

	struct kiumd_user kiusr;
	struct dma_buf_attachment *dmabufattach = NULL;
	struct dma_buf *kiumd_dmabuf = NULL;
    if (copy_from_user(&kiusr, arg, sizeof(struct kiumd_user)))
		return -EFAULT;

	dmabufattach = (struct dma_buf_attachment *)kiusr.dmabufattach;
    dma_buf_unmap_attachment(dmabufattach, (struct sg_table *)kiusr.sgt_ptr,
							DMA_BIDIRECTIONAL);
	kiumd_dmabuf = (struct dma_buf *)kiusr.dmabuf_ptr;
    dma_buf_detach(kiumd_dmabuf, dmabufattach);
    return 0;
}

int kiumd_export_fd(struct kiumd_dev *ki_dev, char __user *arg)
{
	struct kiumd_user kiusr;

	if (copy_from_user(&kiusr, arg, sizeof(struct kiumd_user)))
		return -EFAULT;
	// mutex_lock
	//dmabuf_tbl[g_counter].kiumd_dmabuf = dma_buf_get(kiusr.dma_buf_fd);
	//dmabuf_tbl[g_counter].token = g_counter; // to be changed to hash(g_counter)
	//kiusr.buf_token =  g_counter;
	kiusr.dmabuf_ptr = dma_buf_get(kiusr.dma_buf_fd);
	pr_err("%s:kiusr.buf_token value... %x \n",__func__, kiusr.dmabuf_ptr);
	// mutex_unlock
	if (copy_to_user(arg, &kiusr, sizeof(kiusr))) {
		pr_err("%s: copy_to_user failed... \n",__func__);
		return -EFAULT;
	}

	return 0;
}

/*
 */
struct dma_buf *find_dmabuf(int token)
{
	if(token <1024)
	{
		if (dmabuf_tbl[token].token == token)
			return dmabuf_tbl[token].kiumd_dmabuf;
	}
	pr_err("%s: Token invalid... \n",__func__);
	return NULL;
}

int kiumd_import_fd(struct kiumd_dev *ki_dev, char __user *arg)
{
	struct kiumd_user kiusr;
	struct dma_buf *kiumd_dmabuf = NULL;

	if (copy_from_user(&kiusr, arg, sizeof(struct kiumd_user)))
		return -EFAULT;

	//kiumd_dmabuf = find_dmabuf(kiusr.dmabuf_ptr);
	if(kiumd_dmabuf == NULL) {
		pr_err("%s: find_dmabuf failed returned NULL buffer... \n",__func__);
		return -EFAULT;
	}
	kiusr.dma_buf_fd = dma_buf_fd(kiusr.dmabuf_ptr, (O_CLOEXEC));
	pr_err("%s: copy_to_user failed... FD %x \n",__func__, kiusr.dma_buf_fd);
	if (copy_to_user(arg, &kiusr, sizeof(kiusr))) {
		pr_err("%s: copy_to_user failed... \n",__func__);
		return -EFAULT;
	}

	return 0;
}


int kiumd_dmabuf_fd(struct kiumd_dev *ki_dev, char __user *arg)
{
	struct kiumd_user kiusr;
	struct dma_buf *kiumd_dmabuf = NULL;

	if (copy_from_user(&kiusr, arg, sizeof(struct kiumd_user)))
		return -EFAULT;
	// struct device st
	//dma_buf_get (fd ) -> dma_buf*


	// find new fd
	if (kiusr.flag == 1)
	{
	pr_err("%s:Calling  dma_buf_fd \n",__func__);
	pr_err("%s:Calling  dma_buf_get %x \n",__func__, kiusr.dmabuf_ptr);
	kiusr.heap_fd = dma_buf_fd(kiusr.dmabuf_ptr, (O_CLOEXEC));
	//importer_test(kiumd_dmabuf);
	}
	else {
			pr_err("%s:kiumd_dmabuf_fd %d   %d\n",__func__, kiusr.dma_buf_fd, kiusr.heap_fd);
		pr_err("%s:Calling  dma_buf_get \n",__func__);
		kiumd_dmabuf = dma_buf_get(kiusr.dma_buf_fd);
		pr_err("%s:Calling  dma_buf_get %x \n",__func__, kiumd_dmabuf);
		//kiumd_dmabuf = dma_buf_get(kiusr.dma_buf_fd);
		if(IS_ERR_OR_NULL(kiumd_dmabuf)) {
		pr_err("%s:kiumd_dmabuf is NULL \n",__func__);
		return -ENOTTY;
		}
		kiusr.dmabuf_ptr = kiumd_dmabuf;
	}

	pr_err("%s:Calling  copy_to_user \n",__func__);
	if (copy_to_user(arg, &kiusr, sizeof(kiusr))) {
		//dma_buf_put(kiumd_dmabuf);
		pr_err("%s: copy_to_user failed... \n",__func__);
		return -EFAULT;
	}


	return 0;
}

static int kiumd_open(struct inode *inode, struct file *filp)
{
	pr_err("kiumd_open called\n");
	return 0;
}

static long kiumd_ioctl(struct file *file, unsigned int cmd,
				 unsigned long arg)
{
	struct kiumd_dev *ki_dev = (struct kiumd_dev *)file->private_data;
	char __user *argp = (char __user *)arg;
	int err;
	pr_err("kiumd_ioctl called\n");
	switch (cmd) {
	case KIUMD_SMMU_MAP_BUF:
		err = kiumd_dmabuf_vfio_map(ki_dev, argp);
		break;
	case KIUMD_SMMU_UNMAP_BUF:
		err = kiumd_dmabuf_vfio_unmap(ki_dev, argp);
		break;
	case KIUMD_EXPORT_DMABUF:
		err = kiumd_export_fd(ki_dev, argp);
		break;
	case KIUMD_IMPORT_DMABUF:
		err = kiumd_import_fd(ki_dev, argp);
		break;
	default:
		err = -ENOTTY;
		break;
	}

	return err;
}

static const struct file_operations kiumd_fops = {
	.open = kiumd_open,
	.unlocked_ioctl = kiumd_ioctl,
	.compat_ioctl = kiumd_ioctl,
};

static int kiumd_init(void)
{
	int err;
	char *devname = "kiumd";
	struct kiumd_dev *kidev = NULL;
	kidev = kzalloc(sizeof(struct kiumd_dev), GFP_KERNEL);
	if (!kidev)
		return -ENOMEM;
	kidev->miscdev.minor = MISC_DYNAMIC_MINOR;
	kidev->miscdev.name = devname;
	kidev->miscdev.fops = &kiumd_fops;
	err = misc_register(&kidev->miscdev);
	if (err) {
		pr_err("kiumd misc device creation failure\n");
		return err;
	}

	return 0;
}
module_init(kiumd_init);
MODULE_IMPORT_NS(DMA_BUF);
MODULE_DESCRIPTION("KIUMD");
MODULE_LICENSE("GPL v2");
