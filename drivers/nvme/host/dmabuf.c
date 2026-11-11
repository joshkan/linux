// SPDX-License-Identifier: GPL-2.0
/*
 * dma-buf I/O context handling shared by the NVMe transports.
 * Copyright (C) 2026 Pavel Begunkov <asml.silence@gmail.com>
 */

#include <linux/dma-resv.h>

#include "dmabuf.h"

static void nvme_dmabuf_invalidate_mappings(struct dma_buf_attachment *attach)
{
	struct dma_buf_io_ctx *ctx = attach->importer_priv;

	dma_buf_io_invalidate_mappings(ctx);
}

static const struct dma_buf_attach_ops nvme_dmabuf_importer_ops = {
	.invalidate_mappings	= nvme_dmabuf_invalidate_mappings,
	.allow_peer2peer	= true,
};

void nvme_dma_buf_io_release(struct dma_buf_io_ctx *base_ctx)
{
	struct nvme_dma_buf_io_ctx *ctx = base_ctx->dev_priv;
	struct nvme_dma_buf_list *buf_list = ctx->buf_list;

	mutex_lock(&buf_list->lock);
	if (!list_empty(&ctx->list)) {
		dma_buf_detach(ctx->base->dmabuf, ctx->attach);
		list_del_init(&ctx->list);
	}
	mutex_unlock(&buf_list->lock);

	put_device(ctx->dma_dev);
	nvme_put_ctrl(ctx->ctrl);
	kfree(ctx);
}
EXPORT_SYMBOL_GPL(nvme_dma_buf_io_release);

void __nvme_remove_dmabufs(struct nvme_dma_buf_list *list)
{
	struct nvme_dma_buf_io_ctx *ctx, *tmp;

	mutex_lock(&list->lock);
	list->dead = true;
	list_for_each_entry_safe(ctx, tmp, &list->ctxs, list) {
		dma_buf_io_detach(ctx->base);
		dma_buf_detach(ctx->base->dmabuf, ctx->attach);
		list_del_init(&ctx->list);
	}
	mutex_unlock(&list->lock);
}
EXPORT_SYMBOL_GPL(__nvme_remove_dmabufs);

int nvme_init_dma_buf_io_ctx_locked(struct nvme_ctrl *ctrl,
		struct nvme_dma_buf_list *buf_list, struct device *dma_dev,
		const struct dma_buf_io_ops *ops, struct dma_buf_io_ctx *ctx)
{
	struct nvme_dma_buf_io_ctx *nvme_ctx;
	struct dma_buf_attachment *attach;

	lockdep_assert_held(&buf_list->lock);

	if (buf_list->dead)
		return -ENODEV;

	nvme_ctx = kzalloc_obj(*nvme_ctx);
	if (!nvme_ctx)
		return -ENOMEM;

	attach = dma_buf_dynamic_attach(ctx->dmabuf, dma_dev,
					&nvme_dmabuf_importer_ops, ctx);
	if (IS_ERR(attach)) {
		kfree(nvme_ctx);
		return PTR_ERR(attach);
	}

	nvme_get_ctrl(ctrl);
	list_add(&nvme_ctx->list, &buf_list->ctxs);
	nvme_ctx->attach = attach;
	nvme_ctx->base = ctx;
	nvme_ctx->ctrl = ctrl;
	nvme_ctx->dma_dev = get_device(dma_dev);
	nvme_ctx->buf_list = buf_list;
	ctx->dev_priv = nvme_ctx;
	ctx->dev_ops = ops;
	return 0;
}
EXPORT_SYMBOL_GPL(nvme_init_dma_buf_io_ctx_locked);

/*
 * Count the DMA segments covering @len bytes at @offset into the mapped
 * buffer.  Returns 0 if the buffer is too short.
 */
unsigned int nvme_dmabuf_sgt_nents(struct sg_table *sgt, size_t offset,
		size_t len)
{
	struct scatterlist *sg;
	unsigned int nents = 0;
	unsigned long tmp;

	for_each_sgtable_dma_sg(sgt, sg, tmp) {
		size_t sg_len = sg_dma_len(sg);

		if (!len)
			break;
		if (offset >= sg_len) {
			offset -= sg_len;
			continue;
		}

		sg_len -= offset;
		offset = 0;
		nents++;
		len -= min(len, sg_len);
	}

	if (unlikely(len))
		return 0;
	return nents;
}
EXPORT_SYMBOL_GPL(nvme_dmabuf_sgt_nents);
