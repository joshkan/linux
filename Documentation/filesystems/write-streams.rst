.. SPDX-License-Identifier: GPL-2.0

=============
Write streams
=============

Problem
=======

An application often knows which of its files are written together and
deleted together: a log and the tables built from it, cache segments
of one generation, backups taken at the same time.  The filesystem and
the storage device below it do not know this.  They place data by
arrival order and by their own heuristics, so data with different
lifetimes ends up interleaved on the media.

That interleaving has two costs:

- In the filesystem, unrelated writers allocate from the same
  allocation groups and contend for the same locks.

- In a flash device, an erase unit holds both live and dead data when
  it is reclaimed.  The device must copy the live data out first.  The
  extra writes (write amplification) cost bandwidth, latency and media
  life.

Write streams let the application pass that knowledge down.  A stream
names a group of files that belong together.  It says nothing about
layout; each layer that can act on it (filesystem, block device, RAID)
decides how to keep the group apart from other groups.

Concepts
========

Stream
  A group of files that the application wants placed together and
  apart from other groups.  Inside the kernel a stream is a small
  integer id, starting at 1.  The id is never visible to userspace.

Stream pool
  The set of streams a filesystem offers for one storage target.  Its
  size is chosen by the filesystem when it is mounted.  A filesystem
  with more than one data device (for example an XFS realtime volume)
  has one pool per device.

Stream fd
  A file descriptor that names one reserved stream.  Holding the fd
  keeps the stream reserved.  Applications refer to streams only
  through these fds, so two unrelated applications cannot end up on the
  same stream by picking the same number.

Interface
=========

Three ioctls, issued on an open regular file or directory in the
filesystem::

  FS_IOC_WRITE_STREAM_GET_MAX  number of streams the filesystem offers
  FS_IOC_WRITE_STREAM_ALLOC    reserve a stream, returns a stream fd
  FS_IOC_WRITE_STREAM_SET      set or clear the stream of a file

``FS_IOC_WRITE_STREAM_GET_MAX``
  Returns, through a ``__u32``, the size of the stream pool that serves
  the file.  Zero means write streams are not available for that file.

``FS_IOC_WRITE_STREAM_ALLOC``
  Reserves a free stream from the pool that serves the file and returns
  a new ``O_CLOEXEC`` stream fd as the ioctl return value.  The stream
  stays reserved until the last reference to that fd is closed.
  Requires ``CAP_SYS_ADMIN``.  Fails with ``EBUSY`` when every stream
  is reserved and with ``EOPNOTSUPP`` when the pool is empty.

``FS_IOC_WRITE_STREAM_SET``
  Takes ``struct fs_write_stream_set``::

    struct fs_write_stream_set {
            __s32   stream_fd;      /* stream to attach, -1 to clear */
            __u32   flags;          /* FS_WRITE_STREAM_SET_* */
    };

  Attaches the stream named by ``stream_fd`` to the file the ioctl is
  issued on.  One stream fd may be attached to any number of files.
  With ``FS_WRITE_STREAM_SET_CLEAR`` and a ``stream_fd`` of -1 it
  detaches whatever stream the file has.  The file must be a regular
  file opened for writing, and the caller must own it or be
  capable.  The stream fd must come from the pool that serves the file,
  otherwise the call fails with ``EINVAL``.

A typical application reserves one stream per lifetime class at
startup and attaches the matching stream to each file as it creates
it::

  int fd = open("wal-000123", O_CREAT | O_WRONLY, 0644);
  int log = ioctl(fd, FS_IOC_WRITE_STREAM_ALLOC);  /* once, at startup */
  struct fs_write_stream_set set = { .stream_fd = log };
  ioctl(fd, FS_IOC_WRITE_STREAM_SET, &set);

Semantics
=========

A stream is a placement hint
----------------------------

A stream directs where allocation starts; it never limits where it may
end.  When the space backing a stream is full, the filesystem allocates
elsewhere as it would without a stream.  A write never fails because of
the stream attached to its file, and a single stream can use the whole
filesystem.

Hard placement, where a file must stay within a given part of the
filesystem and allocation fails with ``ENOSPC`` once that part is full,
is a different contract and is not provided by write streams.  It is
filesystem specific (an XFS allocation group number has no meaning on
another filesystem) and belongs in a filesystem specific interface.

.. XXX(kanchan): link Dave's v5 thread and your reply here, or keep
   this to the cover letter?

Lifetime
--------

The stream attached to a file is kept in the in-memory inode only.  It
is not written to disk.

.. XXX(kanchan): decide and document.  Today SET does not pin the
   inode, so the attachment is silently lost when the inode is evicted
   and read back, and after a remount.  Options: (a) document it and
   tell applications to SET again on every open; (b) pin the inode
   while a stream is attached; (c) persist it.  (c) is an on-disk
   format change.

Closing a stream fd releases the reservation but does not detach the
stream from files it was attached to.  Those files keep their id, and
the id may be handed to the next caller of ``FS_IOC_WRITE_STREAM_ALLOC``.
Applications should keep a stream fd open for as long as they write to
files that carry its stream.

.. XXX(kanchan): this is the id-reuse case Sashiko flagged on v5 (#7).
   Confirm that "keep the fd open" is the contract, or detach on close.

Interaction with other placement controls
-----------------------------------------

A file carries either a write stream or a write life time hint
(``F_SET_RW_HINT``), never both.  Setting one while the other is set
fails with ``EBUSY``.

The per-I/O write stream that io_uring can pass in
``ki_write_stream`` is used only for raw block devices.  For a file,
the stream attached to the file applies and the per-I/O value is
ignored, so stream ids never cross the user interface.

Filesystems may refuse streams on files that use their own placement
policy.  See the XFS section below.

Hardware write streams
======================

Some block devices accept a write stream id on each write and place
data with different ids in separate parts of the media.  NVMe devices
with Flexible Data Placement (FDP) are one example.  The block layer
reports how many such streams a device has as
``max_write_streams``; zero means none.

When the device under a stream pool has hardware streams, the
filesystem may size its pool to match and pass the id of each file's
stream down with every write to that file.  Data of one stream then
shares erase units only with data of the same stream.

On a device without hardware streams the filesystem keeps the id to
itself.  It must not reach the block layer, where it could be
misread as a hardware placement handle.

.. XXX(kanchan): the last paragraph describes the v7 partition fix
   (tag bios only when bdev_max_write_streams() != 0).

XFS
===

Pools
-----

XFS keeps one pool for the data device, sized from the number of
allocation groups (AGs), and one for the realtime device when the
filesystem has realtime groups (RTGs) and is not zoned.  A file uses
the pool of the device that holds its data.

Without hardware streams the pool size is derived from the group
count:

=============  ===================
groups         streams
=============  ===================
1 to 7         one per group
8 to 15        one per 2 groups
16 or more     one per 4 groups
=============  ===================

With hardware streams the pool has as many streams as the device, but
no more than the number of groups, so that every stream has at least
one group of its own.  Make the filesystem with at least as many
groups as the device has streams to use them all.  Both counts are
capped at 255.

Placement
---------

The groups are split into equal, contiguous sets, one per stream; the
last stream also gets any remainder.  When a file with a stream needs
its first data extent, XFS starts the search in that stream's set, at
the group chosen by the inode number within the set.  Files of one
stream thus spread over its groups, which keeps concurrency within a
stream.  Later extents of the file follow the usual allocation rules
and stay next to the file's existing data where possible.

If the set is full, allocation continues in the rest of the filesystem
as it would for a file without a stream.

Exclusions
----------

Write streams and the XFS filestreams allocator are mutually
exclusive.  ``FS_IOC_WRITE_STREAM_GET_MAX`` reports zero for a
filestream inode (every inode when mounted with ``-o filestreams``)
and ``FS_IOC_WRITE_STREAM_SET`` refuses it.  Setting the filestream
flag, or changing the realtime flag, of a file that has a stream fails
with ``EINVAL``.  Files on a zoned realtime device do not support
write streams.

.. XXX(kanchan): directories.  Streams apply to regular files only;
   directory blocks and new inodes do not follow a directory's stream.
   Say whether that is a non-goal or future work.

Implementing write streams in a filesystem
==========================================

The ioctl argument and permission checks, the reservation and the fd
handling are generic, in ``fs/write_streams.c``.  A filesystem:

- embeds a ``struct write_stream_pool`` in whatever object scopes an id
  space (XFS uses its buffer target) and sizes it with
  ``write_stream_pool_init()`` at mount;
- calls ``write_stream_ioctl()`` from its ioctl handler for the three
  commands, passing a callback that returns the pool serving an inode,
  or NULL if the inode cannot use write streams.  For ``SET`` the
  callback runs with the inode lock held, which also serializes it
  against ``FS_IOC_FSSETXATTR``;
- uses ``inode->i_write_stream``, which the VFS sets, in its allocator;
- sets ``iomap->write_stream`` (or ``bio->bi_write_stream``) only when
  the device has hardware streams;
- frees the pool with ``write_stream_pool_destroy()`` once no stream fd
  can still refer to it.  A stream fd pins the mount it was allocated
  from, so freeing at unmount is enough.
