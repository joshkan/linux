/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026 Pavel Begunkov <asml.silence@gmail.com>
 */

#ifndef _NVME_PCI_DMABUF_H
#define _NVME_PCI_DMABUF_H

#include "dmabuf.h"

struct nvme_dmabuf_map {
	struct dma_buf_io_map base;
	struct sg_table *sgt;
	unsigned nr_entries;
	dma_addr_t dma_list[] __counted_by(nr_entries);
};

static inline struct nvme_dmabuf_map *to_nvme_dmabuf_map(
	struct dma_buf_io_map *map)
{
	return container_of(map, struct nvme_dmabuf_map, base);
}

static inline struct nvme_dmabuf_map *nvme_req_to_map(struct request *req)
{
	struct dma_buf_io_map *map = req->bio->bi_dmabuf_map;

	return to_nvme_dmabuf_map(map);
}

extern const struct dma_buf_io_ops nvme_pci_dma_buf_io_ops;

#endif /* _NVME_PCI_DMABUF_H */