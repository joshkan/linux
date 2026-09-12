/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Common infrastructure for supporing dma-buf in the I/O path.
 *
 * Copyright (C) 2026 Pavel Begunkov <asml.silence@gmail.com>
 */
#include <linux/dma-buf-io.h>
#include <linux/dma-resv.h>

/*
 * An exporter is free to call dma_buf_invalidate_mappings() from its own
 * eviction/move path and can wait for ->unmap() within bounded time.
 * Queue the resulting deferred unmap work here instead of on system_wq, so
 * unrelated GFP_KERNEL-allocating work saturating the shared pool cannot
 * stall it.
 */
static struct workqueue_struct *dma_buf_io_wq;

struct dma_buf_io_fence {
	struct dma_fence base;
	spinlock_t lock;
};

static const char *dma_buf_io_fence_drv_name(struct dma_fence *fence)
{
	/* default fence release kfree's the base pointer */
	BUILD_BUG_ON(offsetof(struct dma_buf_io_fence, base));

	return "dma-buf-io-ctx";
}

static const char *dma_buf_io_fence_timeline_name(struct dma_fence *fence)
{
	return "dma-buf-io-ctx";
}

const struct dma_fence_ops dma_buf_io_fence_ops = {
	.get_driver_name = dma_buf_io_fence_drv_name,
	.get_timeline_name = dma_buf_io_fence_timeline_name,
};

static void dma_buf_io_ctx_destroy_work(struct work_struct *work)
{
	struct dma_buf_io_ctx *ctx = container_of(work, struct dma_buf_io_ctx,
						  release_work);

	if (WARN_ON_ONCE(refcount_read(&ctx->refs)))
		return;

	ctx->dev_ops->release(ctx);
	dma_buf_put(ctx->dmabuf);
	kfree(ctx);
}

static void dma_buf_io_map_release_work(struct work_struct *work)
{
	struct dma_buf_io_map *map = container_of(work, struct dma_buf_io_map,
						  release_work);
	struct dma_buf_io_fence *fence = map->fence;
	struct dma_buf_io_ctx *ctx = map->ctx;
	struct dma_buf *dmabuf = ctx->dmabuf;

	/* the release path must wait for fences */
	if (WARN_ON_ONCE(refcount_read(&ctx->refs) == 0))
		return;

	/* Prevent from destoying the ctx while unmapping */
	refcount_inc(&ctx->refs);

	/*
	 * The fence, if any, was already signaled from
	 * dma_buf_io_map_refs_release(). This work item carries no
	 * signalling obligations of its own, so it may take the resv lock
	 * and block freely.
	 */
	dma_resv_lock(dmabuf->resv, NULL);
	ctx->dev_ops->unmap(ctx, map);
	dma_resv_unlock(dmabuf->resv);

	if (fence)
		dma_fence_put(&fence->base);
	percpu_ref_exit(&map->refs);
	kfree(map);

	if (refcount_dec_and_test(&ctx->refs)) {
		/*
		 * Destruction needs to wait for I/O and dma fences. Defer it to
		 * simplify locking.
		 */
		INIT_WORK(&ctx->release_work, dma_buf_io_ctx_destroy_work);
		queue_work(system_wq, &ctx->release_work);
	}
}

static void dma_buf_io_map_refs_release(struct percpu_ref *ref)
{
	struct dma_buf_io_map *map = container_of(ref, struct dma_buf_io_map, refs);
	struct dma_buf_io_fence *fence = map->fence;

	/*
	 * dma_fence_signal() and complete() never block or allocate and are
	 * safe from any context, including atomic/IRQ. Signal right here,
	 * synchronously, so the signal has no dependency on workqueue
	 * scheduling: a shared worker pool is itself a resource a fence must
	 * never depend on to make progress, since every worker could be
	 * stuck in reclaim waiting on this very fence. @fence is NULL when
	 * dma_buf_io_drop_map() could not reserve a fence slot; wake its
	 * synchronous waiter unconditionally instead.
	 */
	if (fence)
		dma_fence_signal(&fence->base);
	complete(&map->release_done);

	/* Everything else needs process context: defer it. */
	INIT_WORK(&map->release_work, dma_buf_io_map_release_work);
	queue_work(dma_buf_io_wq, &map->release_work);
}

int dma_buf_io_init_map(struct dma_buf_io_ctx *ctx, struct dma_buf_io_map *map)
{
	struct dma_buf_io_fence *fence;
	int ret;

	ret = percpu_ref_init(&map->refs, dma_buf_io_map_refs_release, 0, GFP_KERNEL);
	if (ret)
		return ret;

	/*
	 * Pre-allocate the fence's backing memory here, at ordinary map
	 * creation, rather than in dma_buf_io_drop_map(). That path is
	 * reached via dma_buf_invalidate_mappings(), which an exporter is
	 * free to call from a reclaim-adjacent context, so it should need to
	 * allocate as little as possible. dma_fence_init() itself still
	 * happens there, only once a slot for it has been reserved in the
	 * dmabuf's reservation object: initializing it here instead would
	 * leave it live (and thus subject to the "no allocations until this
	 * fence signals" dma_fence rule) for the map's entire, arbitrarily
	 * long, active lifetime.
	 */
	fence = kzalloc(sizeof(*fence), GFP_KERNEL);
	if (!fence) {
		percpu_ref_exit(&map->refs);
		return -ENOMEM;
	}

	init_completion(&map->release_done);
	map->fence = fence;
	map->ctx = ctx;
	return 0;
}
EXPORT_SYMBOL_NS_GPL(dma_buf_io_init_map, "DMA_BUF");

struct dma_buf_io_map *dma_buf_io_create_map(struct dma_buf_io_ctx *ctx)
{
	struct dma_buf *dmabuf = ctx->dmabuf;
	struct dma_buf_io_map *map;
	long ret;

retry:
	/*
	 * ->dmabuf_map() will be calling dma_buf_map_attachment(), for which
	 * we'll need to wait for fences. Do a bit nicer and try to wait
	 * without the resv lock first.
	 */
	ret = dma_resv_wait_timeout(dmabuf->resv, DMA_RESV_USAGE_KERNEL,
				    true, MAX_SCHEDULE_TIMEOUT);
	if (!ret)
		ret = -EAGAIN;
	if (ret < 0)
		return ERR_PTR(ret);

	ret = dma_resv_lock_interruptible(dmabuf->resv, NULL);
	if (ret)
		return ERR_PTR(ret);

	map = dma_buf_io_get_map(ctx);
	if (map) {
		ret = 0;
		goto out;
	}

	if (dma_resv_wait_timeout(dmabuf->resv, DMA_RESV_USAGE_KERNEL,
				  true, 0) < 0) {
		dma_resv_unlock(dmabuf->resv);
		goto retry;
	}

	map = ctx->dev_ops->map(ctx);
	if (IS_ERR(map)) {
		ret = PTR_ERR(map);
		goto out;
	}

	percpu_ref_get(&map->refs);
	rcu_assign_pointer(ctx->map, map);
out:
	dma_resv_unlock(dmabuf->resv);
	if (ret < 0)
		return ERR_PTR(ret);
	return map;
}

static void dma_buf_io_drop_map(struct dma_buf_io_ctx *ctx)
{
	struct dma_buf *dmabuf = ctx->dmabuf;
	struct dma_buf_io_map *map;
	struct dma_buf_io_fence *fence;
	int ret;

	dma_resv_assert_held(dmabuf->resv);

	map = rcu_dereference_protected(ctx->map,
					dma_resv_held(dmabuf->resv));
	if (!map)
		return;
	rcu_assign_pointer(ctx->map, NULL);
	fence = map->fence;

	/*
	 * Follow the dma_fence rules: the fence's memory is already
	 * allocated (dma_buf_io_init_map()); reserve a slot for it and only
	 * then init the dma_fence itself. No allocation is allowed between
	 * dma_fence_init() and dma_resv_add_fence() below, since once the
	 * fence exists it can in principle be found and waited on, and
	 * memory reclaim looping back to wait for it would deadlock.
	 */
	ret = dma_resv_reserve_fences(dmabuf->resv, 1);
	if (WARN_ON_ONCE(ret)) {
		/* Fence was never initialized; just free its memory. */
		kfree(fence);
		map->fence = NULL;
		goto wait_sync;
	}

	spin_lock_init(&fence->lock);
	dma_fence_init(&fence->base, &dma_buf_io_fence_ops, &fence->lock,
			ctx->fence_ctx, atomic_inc_return(&ctx->fence_seq));

	dma_resv_add_fence(dmabuf->resv, &fence->base, DMA_RESV_USAGE_KERNEL);
	/*
	 * Delay destruction until all inflight requests using the map are
	 * gone. It'll also signal the fence then.
	 */
	percpu_ref_kill(&map->refs);
	return;

wait_sync:
	/*
	 * Couldn't reserve a fence slot (OOM). Nothing was published to the
	 * reservation object, so wait synchronously for in-flight users to
	 * drop the map instead of relying on dma_resv to signal completion.
	 */
	percpu_ref_kill(&map->refs);
	wait_for_completion(&map->release_done);
}

void dma_buf_io_invalidate_mappings(struct dma_buf_io_ctx *ctx)
{
	dma_buf_io_drop_map(ctx);
}
EXPORT_SYMBOL_NS_GPL(dma_buf_io_invalidate_mappings, "DMA_BUF");

static void dma_buf_io_ctx_release_work(struct work_struct *work)
{
	struct dma_buf_io_ctx *ctx = container_of(work, struct dma_buf_io_ctx,
						  release_work);
	struct dma_buf *dmabuf = ctx->dmabuf;
	long ret;

	dma_resv_lock(dmabuf->resv, NULL);
	/* Remove the last map, there should be no new ones going forward. */
	dma_buf_io_drop_map(ctx);
	dma_resv_unlock(dmabuf->resv);

	/* Wait until all maps are destroyed. */
	ret = dma_resv_wait_timeout(dmabuf->resv, DMA_RESV_USAGE_KERNEL,
				    false, MAX_SCHEDULE_TIMEOUT);

	if (WARN_ON_ONCE(ret <= 0))
		return;
	if (WARN_ON_ONCE(rcu_dereference_protected(ctx->map, true)))
		return;

	if (refcount_dec_and_test(&ctx->refs))
		dma_buf_io_ctx_destroy_work(&ctx->release_work);
}

void dma_buf_io_ctx_release(struct dma_buf_io_ctx *ctx)
{
	/*
	 * Destruction needs to wait for I/O and dma fences. Defer it to
	 * simplify locking.
	 */
	INIT_WORK(&ctx->release_work, dma_buf_io_ctx_release_work);
	queue_work(system_wq, &ctx->release_work);
}

int dma_buf_io_ctx_create(struct file *file,
			   struct dma_buf_io_ctx *ctx,
			   struct dma_buf *dmabuf,
			   enum dma_data_direction dir)
{
	int ret;

	if (!file->f_op->init_dma_buf_io_ctx)
		return -EOPNOTSUPP;

	memset(ctx, 0, sizeof(*ctx));
	ctx->fence_ctx = dma_fence_context_alloc(1);
	ctx->dir = dir;
	ctx->dmabuf = dmabuf;
	refcount_set(&ctx->refs, 1);
	get_dma_buf(dmabuf);

	ret = file->f_op->init_dma_buf_io_ctx(file, ctx);
	if (ret) {
		memset(ctx, 0, sizeof(*ctx));
		dma_buf_put(dmabuf);
		return ret;
	}

	if (WARN_ON_ONCE(!ctx->dev_ops ||
			 !ctx->dev_ops->map ||
			 !ctx->dev_ops->unmap ||
			 !ctx->dev_ops->release))
		return -EINVAL;

	return ret;
}

static int __init dma_buf_io_init(void)
{
	dma_buf_io_wq = alloc_workqueue("dma_buf_io", WQ_MEM_RECLAIM | WQ_UNBOUND, 0);
	if (!dma_buf_io_wq)
		return -ENOMEM;
	return 0;
}
subsys_initcall(dma_buf_io_init);
