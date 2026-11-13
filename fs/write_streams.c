// SPDX-License-Identifier: GPL-2.0
/* Generic write-stream fd management. */
#include <linux/anon_inodes.h>
#include <linux/bitmap.h>
#include <linux/capability.h>
#include <linux/err.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/mount.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/write_streams.h>

#include "internal.h"

struct write_stream {
	struct write_stream_pool	*pool;
	struct vfsmount			*mnt;	/* pins the mount */
	u16				id;	/* 1-based slot number */
};

static int write_stream_release(struct inode *inode, struct file *file)
{
	struct write_stream *ws = file->private_data;

	/* ws->mnt keeps the pool alive until the mntput() below */
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
 * @nr: number of slots, may be zero, at most U16_MAX
 *
 * Return: 0, -EINVAL if @nr is too large, or -ENOMEM if the bitmap cannot be
 * allocated.
 */
int write_stream_pool_init(struct write_stream_pool *pool, unsigned int nr)
{
	spin_lock_init(&pool->lock);
	pool->streams_in_use = NULL;
	pool->nr_streams = 0;

	if (nr > U16_MAX)
		return -EINVAL;
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
 * @pool: pool to release
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
 * @pool: pool to reserve from; must live as long as the mount of @file
 * @file: file whose mount is pinned for the life of the stream
 *
 * Return: an O_RDONLY | O_CLOEXEC fd, -EOPNOTSUPP if the pool has no slots,
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

/**
 * write_stream_get_id - the id a stream file names
 * @file: candidate stream file
 * @pool: pool the stream must belong to, or NULL to accept any
 *
 * Return: the 1-based id, or -EINVAL if @file is not a stream file, or is
 * a stream of a different pool.
 */
int write_stream_get_id(struct file *file, const struct write_stream_pool *pool)
{
	struct write_stream *ws;

	if (file->f_op != &write_stream_fops)
		return -EINVAL;
	ws = file->private_data;
	if (pool && ws->pool != pool)
		return -EINVAL;
	return ws->id;
}

/**
 * inode_set_write_stream - attach or clear the write stream of an inode
 * @inode: inode to update
 * @id: stream id, or WRITE_STREAM_NOT_SET to clear
 *
 * A write stream and a write life time hint are mutually exclusive.
 *
 * Return: 0, or -EBUSY if @inode has a write life time hint.
 */
int inode_set_write_stream(struct inode *inode, u16 id)
{
	spin_lock(&inode->i_lock);
	if (id != WRITE_STREAM_NOT_SET &&
	    inode->i_write_hint != WRITE_LIFE_NOT_SET) {
		spin_unlock(&inode->i_lock);
		return -EBUSY;
	}
	WRITE_ONCE(inode->i_write_stream, id);
	spin_unlock(&inode->i_lock);
	return 0;
}

static int ioctl_write_stream_get_max(struct file *file, __u32 __user *argp,
				      write_stream_pool_fn pool_fn)
{
	struct write_stream_pool *pool = pool_fn(file_inode(file));

	return put_user(pool ? write_stream_pool_count(pool) : 0, argp);
}

static int ioctl_write_stream_alloc(struct file *file,
				    write_stream_pool_fn pool_fn)
{
	struct write_stream_pool *pool;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	pool = pool_fn(file_inode(file));
	if (!pool)
		return -EOPNOTSUPP;
	return write_stream_alloc_fd(pool, file);
}

static int ioctl_write_stream_attach(struct inode *inode, int stream_fd,
				     write_stream_pool_fn pool_fn)
{
	CLASS(fd, f)(stream_fd);
	struct write_stream_pool *pool;
	int id, error;

	if (fd_empty(f))
		return -EBADF;

	/*
	 * The inode lock keeps the pool of @inode stable until the id is
	 * attached: vfs_fileattr_set() holds it while flags that select the
	 * pool change.
	 */
	inode_lock(inode);
	pool = pool_fn(inode);
	if (!pool) {
		error = -EINVAL;
		goto out_unlock;
	}
	id = write_stream_get_id(fd_file(f), pool);
	if (id < 0) {
		error = id;
		goto out_unlock;
	}
	error = inode_set_write_stream(inode, id);
out_unlock:
	inode_unlock(inode);
	return error;
}

static int ioctl_write_stream_set(struct file *file,
				  struct fs_write_stream_set __user *argp,
				  write_stream_pool_fn pool_fn)
{
	struct inode *inode = file_inode(file);
	struct fs_write_stream_set set;

	if (!(file->f_mode & FMODE_WRITE))
		return -EBADF;
	if (copy_from_user(&set, argp, sizeof(set)))
		return -EFAULT;
	if (set.flags & ~FS_WRITE_STREAM_SET_CLEAR)
		return -EINVAL;
	if (!S_ISREG(inode->i_mode))
		return -EINVAL;
	if (!inode_owner_or_capable(file_mnt_idmap(file), inode))
		return -EPERM;

	if (!(set.flags & FS_WRITE_STREAM_SET_CLEAR))
		return ioctl_write_stream_attach(inode, set.stream_fd, pool_fn);
	if (set.stream_fd != -1)
		return -EINVAL;

	inode_lock(inode);
	inode_set_write_stream(inode, WRITE_STREAM_NOT_SET);
	inode_unlock(inode);
	return 0;
}

/**
 * write_stream_ioctl - handle the FS_IOC_WRITE_STREAM_* ioctls
 * @file: file the ioctl is issued on
 * @cmd: ioctl command
 * @argp: ioctl argument
 * @pool_fn: returns the pool that serves an inode, or NULL if the inode
 *	cannot use write streams; may sleep, and is called with the inode
 *	lock held for FS_IOC_WRITE_STREAM_SET
 *
 * Called by a filesystem from its ioctl handler.  GET_MAX and ALLOC work on
 * any file the filesystem passes in, SET on regular files only.
 */
long write_stream_ioctl(struct file *file, unsigned int cmd,
			void __user *argp, write_stream_pool_fn pool_fn)
{
	switch (cmd) {
	case FS_IOC_WRITE_STREAM_GET_MAX:
		return ioctl_write_stream_get_max(file, argp, pool_fn);
	case FS_IOC_WRITE_STREAM_ALLOC:
		return ioctl_write_stream_alloc(file, pool_fn);
	case FS_IOC_WRITE_STREAM_SET:
		return ioctl_write_stream_set(file, argp, pool_fn);
	}
	return -ENOTTY;
}
EXPORT_SYMBOL_GPL(write_stream_ioctl);
