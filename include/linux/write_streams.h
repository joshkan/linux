/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_WRITE_STREAMS_H
#define _LINUX_WRITE_STREAMS_H

#include <linux/spinlock.h>
#include <linux/types.h>

struct file;

/*
 * Per-device write stream slot allocator, embedded in the filesystem's device
 * abstraction (e.g. xfs_buftarg) and kept alive by the mount reference each
 * stream holds.  Every access to streams_in_use is under lock, so the
 * non-atomic bitops are enough.
 */
struct write_stream_pool {
	unsigned int		nr_streams;
	unsigned long		*streams_in_use;
	spinlock_t		lock;
};

int  write_stream_pool_init(struct write_stream_pool *pool, unsigned int nr);
void write_stream_pool_destroy(struct write_stream_pool *pool);

int  write_stream_alloc_fd(struct write_stream_pool *pool, struct file *file);
bool write_stream_file_check(struct file *file,
			     const struct write_stream_pool *pool);
u16  write_stream_get_id(struct file *file);

#endif /* _LINUX_WRITE_STREAMS_H */
