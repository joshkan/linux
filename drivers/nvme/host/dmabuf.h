/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026 Pavel Begunkov <asml.silence@gmail.com>
 */

#ifndef _NVME_DMABUF_H
#define _NVME_DMABUF_H

#include <linux/dma-buf-io.h>

#include "nvme.h"

struct nvme_dma_buf_list {
	struct list_head ctxs;
	struct mutex lock;
	bool dead;
};

struct nvme_dma_buf_io_ctx {
	struct dma_buf_attachment *attach;
	struct nvme_dma_buf_list *buf_list;
	struct dma_buf_io_ctx *base;
	struct nvme_ctrl *ctrl;
	/* the device the buffer is mapped for */
	struct device *dma_dev;
	struct list_head list;
};

void nvme_dma_buf_io_release(struct dma_buf_io_ctx *base_ctx);
void __nvme_remove_dmabufs(struct nvme_dma_buf_list *list);
int nvme_init_dma_buf_io_ctx_locked(struct nvme_ctrl *ctrl,
		struct nvme_dma_buf_list *buf_list, struct device *dma_dev,
		const struct dma_buf_io_ops *ops, struct dma_buf_io_ctx *ctx);
unsigned int nvme_dmabuf_sgt_nents(struct sg_table *sgt, size_t offset,
		size_t len);

static inline void nvme_remove_dmabufs(struct nvme_dma_buf_list *list)
{
	if (IS_ENABLED(CONFIG_DMA_SHARED_BUFFER))
		__nvme_remove_dmabufs(list);
}

static inline int nvme_init_dma_buf_io_ctx(struct nvme_ctrl *ctrl,
		struct nvme_dma_buf_list *buf_list, struct device *dma_dev,
		const struct dma_buf_io_ops *ops, struct dma_buf_io_ctx *ctx)
{
	int ret;

	if (!IS_ENABLED(CONFIG_DMA_SHARED_BUFFER))
		return -EOPNOTSUPP;

	mutex_lock(&buf_list->lock);
	ret = nvme_init_dma_buf_io_ctx_locked(ctrl, buf_list, dma_dev, ops,
					      ctx);
	mutex_unlock(&buf_list->lock);
	return ret;
}

static inline void nvme_init_buf_list(struct nvme_dma_buf_list *buf_list)
{
	INIT_LIST_HEAD(&buf_list->ctxs);
	mutex_init(&buf_list->lock);
	buf_list->dead = false;
}

#endif /* _NVME_DMABUF_H */
