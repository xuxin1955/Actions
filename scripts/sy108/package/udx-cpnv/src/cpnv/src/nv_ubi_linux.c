/* SPDX-License-Identifier: GPL-2.0-only */
#define _FILE_OFFSET_BITS 64
#include "nv_ubi_linux.h"
#include <errno.h>
#include <mtd/ubi-user.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

static long read_at(void *context, int fd, void *bytes, size_t length, uint64_t offset)
{
	(void)context;
	ssize_t n = pread(fd, bytes, length, (off_t)offset);
	return n < 0 ? -errno : n;
}

static int update(void *context, int fd, uint64_t bytes)
{
	(void)context;
	int64_t length = (int64_t)bytes;
	return ioctl(fd, UBI_IOCVOLUP, &length) < 0 ? -errno : 0;
}

static long write_next(void *context, int fd, const void *bytes, size_t length)
{
	(void)context;
	ssize_t n = write(fd, bytes, length);
	return n < 0 ? -errno : n;
}

static int sync_volume(void *context, int fd)
{
	(void)context;
	return fsync(fd) < 0 ? -errno : 0;
}

static int prepare_direct(void *context, int fd)
{
	(void)context;
	struct ubi_set_vol_prop_req property = {.property = UBI_VOL_PROP_DIRECT_WRITE, .value = 1};
	return ioctl(fd, UBI_IOCSETVOLPROP, &property) < 0 ? -errno : 0;
}

static int write_leb(void *context, int fd, uint64_t offset, const void *data, size_t length)
{
	(void)context;
	int lnum = (int)(offset / length);
	if (ioctl(fd, UBI_IOCEBUNMAP, &lnum))
		return -errno;
	ssize_t n = pwrite(fd, data, length, (off_t)offset);
	return n == (ssize_t)length ? 0 : n < 0 ? -errno : -EIO;
}

static int check_io(void *context)
{
	struct nv_ubi_linux_context *c = context;
	return c->check_io(c->owner);
}

static int now(void *context, uint64_t *out)
{
	struct timespec time;
	(void)context;
	if (clock_gettime(CLOCK_MONOTONIC, &time))
		return -errno;
	*out = (uint64_t)time.tv_sec * 1000 + (uint64_t)time.tv_nsec / 1000000;
	return 0;
}

struct nv_ubi_ops nv_ubi_linux_ops(struct nv_ubi_linux_context *context)
{
	return (struct nv_ubi_ops){.ctx = context,
				   .pread = read_at,
				   .begin_update = update,
				   .write = write_next,
				   .sync = sync_volume,
				   .check_io = check_io,
				   .now_ms = now,
				   .prepare_direct = prepare_direct,
				   .write_leb = write_leb};
}
