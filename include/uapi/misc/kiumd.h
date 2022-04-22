/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note
 * Copyright (c) 2022 Qualcomm Innovation Center, Inc. All rights reserved.
 */
#ifndef __KIUMD_H__
#define __KIUMD_H__

#include <linux/types.h>
#include <linux/unistd.h>

#define KIUMD_SMMU_MAP_BUF		_IOWR('R', 10, struct kiumd_user)
#define KIUMD_SMMU_UNMAP_BUF	_IOWR('R', 11, struct kiumd_user)
#define KIUMD_EXPORT_DMABUF		_IOWR('R', 12, struct kiumd_user)
#define KIUMD_IMPORT_DMABUF		_IOWR('R', 13, struct kiumd_user)


struct kiumd_user {
	int vfio_fd;
	int dma_buf_fd;
	int heap_fd;
	int flag;
	long int sgt_ptr;
	long int dmabuf_ptr;
	long int dmabufattach;
	unsigned long dma_addr;
	int buf_token;
};

#endif /* __KIUMD_H__ */
