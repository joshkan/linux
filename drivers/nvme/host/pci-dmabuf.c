/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Code for supporting dma-buf I/O maps in the NVMe driver.
 * Copyright (C) 2026 Pavel Begunkov <asml.silence@gmail.com>
 */

#include <linux/dma-resv.h>

#include "pci-dmabuf.h"

static struct dma_buf_io_map *nvme_dma_buf_io_map(struct dma_buf_io_ctx *ctx)
{
	unsigned nr_entries = ctx->dmabuf->size / NVME_CTRL_PAGE_SIZE;
	struct nvme_dma_buf_io_ctx *nvme_ctx = ctx->dev_priv;
	struct dma_buf_attachment *attach = nvme_ctx->attach;
	unsigned long tmp, i = 0;
	struct nvme_dmabuf_map *map;
	struct scatterlist *sg;
	struct sg_table *sgt;
	int ret;

	dma_resv_assert_held(ctx->dmabuf->resv);

	if (ctx->dmabuf->size % NVME_CTRL_PAGE_SIZE)
		return ERR_PTR(-EINVAL);

	map = kvmalloc_flex(*map, dma_list, nr_entries);
	if (!map)
		return ERR_PTR(-ENOMEM);
	map->nr_entries = nr_entries;

	sgt = dma_buf_map_attachment(attach, ctx->dir);
	if (IS_ERR(sgt)) {
		ret = PTR_ERR(sgt);
		sgt = NULL;
		goto err;
	}

	for_each_sgtable_dma_sg(sgt, sg, tmp) {
		dma_addr_t dma_addr = sg_dma_address(sg);
		unsigned long sg_len = sg_dma_len(sg);

		if ((sg_len % NVME_CTRL_PAGE_SIZE) ||
		    (dma_addr % NVME_CTRL_PAGE_SIZE)) {
			ret = -EINVAL;
			goto err;
		}
		while (sg_len) {
			map->dma_list[i++] = dma_addr;
			dma_addr += NVME_CTRL_PAGE_SIZE;
			sg_len -= NVME_CTRL_PAGE_SIZE;
		}
	}

	ret = dma_buf_io_init_map(ctx, &map->base, sgt);
	if (ret)
		goto err;
	map->sgt = sgt;
	return &map->base;
err:
	if (sgt)
		dma_buf_unmap_attachment(attach, sgt, ctx->dir);
	kvfree(map);
	return ERR_PTR(ret);
}

static void nvme_dma_buf_io_unmap(struct dma_buf_io_ctx *ctx,
		struct dma_buf_io_map *map_base)
{
	struct nvme_dma_buf_io_ctx *nvme_ctx = ctx->dev_priv;
	struct nvme_dmabuf_map *map = to_nvme_dmabuf_map(map_base);

	dma_resv_assert_held(ctx->dmabuf->resv);

	dma_buf_unmap_attachment(nvme_ctx->attach, map->sgt, ctx->dir);
	kvfree(map);
}

const struct dma_buf_io_ops nvme_pci_dma_buf_io_ops = {
	.map		= nvme_dma_buf_io_map,
	.unmap		= nvme_dma_buf_io_unmap,
	.release	= nvme_dma_buf_io_release,
};
