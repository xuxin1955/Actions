/* SPDX-License-Identifier: GPL-2.0-only */
#include "cpnv_io.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>

/* These errors mean the kernel has not granted a data fd yet. Once opened,
 * a CP data fd belongs to that kernel instance and is never reopened on error. */
bool cpnv_port_pending(int error)
{
	return error == -ENOENT || error == -ENODEV || error == -EPIPE || error == -EAGAIN ||
	       error == -EINTR;
}

int cpnv_port_open(const char *path, int access)
{
	int fd = open(path, access | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return -errno;
	struct stat stat;
	int error = 0;
	if (fstat(fd, &stat))
		error = -errno;
	else if (!S_ISCHR(stat.st_mode) || stat.st_uid || (stat.st_mode & 0002))
		error = -EPERM;
	if (error) {
		close(fd);
		return error;
	}
	return fd;
}

int cpnv_private_read(const char *path, void *data, size_t capacity, bool exact)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return -errno;
	struct stat stat;
	int error = 0;
	if (fstat(fd, &stat))
		error = -errno;
	else if (!S_ISREG(stat.st_mode) || stat.st_uid || stat.st_nlink != 1 ||
		 (stat.st_mode & 0077))
		error = -EPERM;
	else if (stat.st_size <= 0 || (uint64_t)stat.st_size > capacity ||
		 (exact && (uint64_t)stat.st_size != capacity))
		error = -EINVAL;
	size_t used = 0;
	while (!error && used < (size_t)stat.st_size) {
		ssize_t n =
		    pread(fd, (char *)data + used, (size_t)stat.st_size - used, (off_t)used);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0) {
			error = n < 0 ? -errno : -EIO;
			break;
		}
		used += (size_t)n;
	}
	close(fd);
	return error ? error : (int)used;
}

int cpnv_read(const char *path, void *data, size_t capacity)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	ssize_t n;
	do {
		n = pread(fd, data, capacity, 0);
	} while (n < 0 && errno == EINTR);
	int error = errno;
	close(fd);
	return n < 0 ? -error : (int)n;
}
