// SPDX-License-Identifier: GPL-2.0
/*
 * Generic write-stream fd management.
 *
 * A write stream is a lease on a slot in a filesystem's write-stream pool,
 * represented as an anonymous fd; the slot is released when the fd is closed.
 * The filesystem embeds struct write_stream_pool in its device abstraction.
 *
 * Each stream holds a mount reference, so the pool cannot be freed while a
 * stream fd is live.
 */
#include <linux/anon_inodes.h>
#include <linux/bitmap.h>
#include <linux/err.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/mount.h>
#include <linux/slab.h>
#include <linux/write_streams.h>

struct write_stream {
	struct write_stream_pool	*pool;
	struct vfsmount			*mnt;
	u16				id;	/* 1-based slot number */
};

static int write_stream_release(struct inode *inode, struct file *file)
{
	struct write_stream *ws = file->private_data;

	spin_lock(&ws->pool->lock);
	__clear_bit(ws->id - 1, ws->pool->streams_in_use);
	spin_unlock(&ws->pool->lock);
	mntput(ws->mnt);
	kfree(ws);
	return 0;
}

static const struct file_operations write_stream_fops = {
	.release	= write_stream_release,
	.llseek		= noop_llseek,
};

/**
 * write_stream_pool_init - size a write stream pool
 * @pool: pool to initialise
 * @nr: number of slots, may be zero
 *
 * Returns 0, or -ENOMEM if the bitmap cannot be allocated.  The pool is left
 * safe to destroy either way.
 */
int write_stream_pool_init(struct write_stream_pool *pool, unsigned int nr)
{
	spin_lock_init(&pool->lock);
	pool->streams_in_use = NULL;
	pool->nr_streams = 0;

	if (!nr)
		return 0;

	pool->streams_in_use = bitmap_zalloc(nr, GFP_KERNEL);
	if (!pool->streams_in_use)
		return -ENOMEM;
	pool->nr_streams = nr;
	return 0;
}
EXPORT_SYMBOL_GPL(write_stream_pool_init);

/**
 * write_stream_pool_destroy - free a write stream pool
 * @pool: pool to release; may never have been sized
 */
void write_stream_pool_destroy(struct write_stream_pool *pool)
{
	bitmap_free(pool->streams_in_use);
	pool->streams_in_use = NULL;
	pool->nr_streams = 0;
}
EXPORT_SYMBOL_GPL(write_stream_pool_destroy);

/**
 * write_stream_alloc_fd - reserve a slot and return an fd naming it
 * @pool: pool to reserve from
 * @file: file whose mount is pinned for the life of the stream
 *
 * Returns an O_RDONLY | O_CLOEXEC fd, -EOPNOTSUPP if the pool has no slots,
 * or -EBUSY if they are all taken.
 */
int write_stream_alloc_fd(struct write_stream_pool *pool, struct file *file)
{
	struct write_stream *ws;
	unsigned int slot;
	int fd;

	if (!pool->nr_streams)
		return -EOPNOTSUPP;

	ws = kzalloc_obj(*ws, GFP_KERNEL);
	if (!ws)
		return -ENOMEM;

	spin_lock(&pool->lock);
	slot = find_first_zero_bit(pool->streams_in_use, pool->nr_streams);
	if (slot >= pool->nr_streams) {
		spin_unlock(&pool->lock);
		kfree(ws);
		return -EBUSY;
	}
	__set_bit(slot, pool->streams_in_use);
	spin_unlock(&pool->lock);

	ws->pool = pool;
	ws->mnt = mntget(file->f_path.mnt);
	ws->id = slot + 1;

	fd = anon_inode_getfd("[write_stream]", &write_stream_fops, ws,
			      O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		spin_lock(&pool->lock);
		__clear_bit(slot, pool->streams_in_use);
		spin_unlock(&pool->lock);
		mntput(ws->mnt);
		kfree(ws);
	}
	return fd;
}
EXPORT_SYMBOL_GPL(write_stream_alloc_fd);

/**
 * write_stream_file_check - is @file a write stream of @pool?
 * @file: candidate stream file
 * @pool: pool it must belong to
 *
 * Rejects a stream reserved against a different device.
 */
bool write_stream_file_check(struct file *file,
			     const struct write_stream_pool *pool)
{
	struct write_stream *ws;

	if (file->f_op != &write_stream_fops)
		return false;
	ws = file->private_data;
	return ws->pool == pool;
}
EXPORT_SYMBOL_GPL(write_stream_file_check);

/**
 * write_stream_get_id - the 1-based id of a write stream fd
 * @file: stream file, already checked with write_stream_file_check()
 */
u16 write_stream_get_id(struct file *file)
{
	return ((struct write_stream *)file->private_data)->id;
}
EXPORT_SYMBOL_GPL(write_stream_get_id);
