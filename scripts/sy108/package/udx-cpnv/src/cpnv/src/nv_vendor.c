/* SPDX-License-Identifier: GPL-2.0-only */
#include "nv_vendor.h"
#include "nv_codec.h"
#include <errno.h>
#include <mtd/ubi-user.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

uint32_t nv_vendor_payload_size(const uint8_t header[NV_HEADER], uint32_t capacity)
{
	uint32_t size = get32(header + 4);
	return get32(header) == NV_IMAGE_MAGIC && size && size <= capacity ? size : 0;
}

int nv_vendor_valid(const uint8_t *p, size_t n, uint32_t capacity, unsigned *bits)
{
	if (!p || n < NV_HEADER || !capacity || capacity > NV_MAX_IMAGE)
		return 0;
	uint32_t payload = nv_vendor_payload_size(p, capacity);
	if (!payload || payload > n - NV_HEADER)
		return 0;
	uint32_t sum = get32(p + 8);
	unsigned mode;
	if (sum == nv_checksum(p + 512, payload, 64))
		mode = 64;
	else if (sum == nv_checksum(p + 512, payload, 32))
		mode = 32;
	else
		return 0;
	if (bits)
		*bits = mode;
	return 1;
}

static int timely(uint64_t end)
{
	struct timespec t;
	if (clock_gettime(CLOCK_MONOTONIC, &t))
		return -errno;
	uint64_t now = (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
	return now < end ? 0 : -ETIMEDOUT;
}

#define IO_CHUNK 65536u

static int read_at(int fd, uint8_t *p, size_t length, uint64_t base, uint64_t end)
{
	size_t off = 0;
	while (off < length) {
		int e = timely(end);
		if (e)
			return e;
		size_t n = length - off;
		if (n > IO_CHUNK)
			n = IO_CHUNK;
		ssize_t got = pread(fd, p + off, n, (off_t)(base + off));
		if (got < 0 && errno == EINTR)
			continue;
		if (got <= 0)
			return got < 0 ? -errno : -EIO;
		off += (size_t)got;
	}
	return timely(end);
}

static int valid_stored_image(int fd, uint32_t capacity, uint8_t first[4],
			      uint32_t *payload_size, uint64_t end)
{
	uint8_t header[NV_HEADER];
	int e = read_at(fd, header, sizeof(header), 0, end);
	if (e)
		return e;
	uint32_t payload = nv_vendor_payload_size(header, capacity);
	if (!payload || (first && payload < 4))
		return -EBADMSG;
	uint8_t *block = malloc(IO_CHUNK);
	if (!block)
		return -ENOMEM;
	uint64_t sum = 0;
	for (uint32_t off = 0; off < payload;) {
		size_t n = payload - off;
		if (n > IO_CHUNK)
			n = IO_CHUNK;
		if ((e = read_at(fd, block, n, (uint64_t)NV_HEADER + off, end)))
			break;
		if (!off && first)
			memcpy(first, block, 4);
		sum += nv_checksum_sum(block, n);
		off += (uint32_t)n;
	}
	if (!e && get32(header + 8) != nv_checksum_finish(sum, 64) &&
	    get32(header + 8) != nv_checksum_finish(sum, 32))
		e = -EBADMSG;
	if (!e && payload_size)
		*payload_size = payload;
	free(block);
	return e;
}

int nv_vendor_repair_pair(const int fd[2], uint32_t payload, uint64_t end)
{
	if (!fd || fd[0] < 0 || fd[1] < 0 || fd[0] == fd[1] || !payload || payload > NV_MAX_IMAGE)
		return -EINVAL;
	int good[2], e = 0;
	uint32_t sizes[2];
	for (unsigned i = 0; i < 2; i++) {
		int r = valid_stored_image(fd[i], payload, NULL, &sizes[i], end);
		if (r == -ETIMEDOUT || r == -ENOMEM)
			return r;
		good[i] = !r;
	}
	if (good[0] && good[1])
		return 0;
	if (!good[0] && !good[1])
		return -EBADMSG;
	unsigned source = good[0] ? 0u : 1u, target = source ^ 1u;
	size_t length = (size_t)sizes[source] + NV_HEADER;
	uint8_t *data = malloc(IO_CHUNK), *check = malloc(IO_CHUNK);
	if (!data || !check) {
		e = -ENOMEM;
		goto out;
	}
	int64_t update = (int64_t)length;
	if ((e = timely(end)))
		goto out;
	/* The caller has stopped CP and exclusively owns both volumes. The valid
	 * source remains stable while the invalid replica is streamed and checked. */
	if (ioctl(fd[target], UBI_IOCVOLUP, &update)) {
		e = -errno;
		goto out;
	}
	for (size_t off = 0; off < length;) {
		size_t n = length - off;
		if (n > IO_CHUNK)
			n = IO_CHUNK;
		if ((e = read_at(fd[source], data, n, off, end)))
			goto out;
		size_t done = 0;
		while (done < n) {
			if ((e = timely(end)))
				goto out;
			ssize_t wrote = write(fd[target], data + done, n - done);
			if (wrote < 0 && errno == EINTR)
				continue;
			if (wrote <= 0) {
				e = wrote < 0 ? -errno : -EIO;
				goto out;
			}
			done += (size_t)wrote;
		}
		off += n;
	}
	if (fsync(fd[target])) {
		e = -errno;
		goto out;
	}
	for (size_t off = 0; off < length;) {
		size_t n = length - off;
		if (n > IO_CHUNK)
			n = IO_CHUNK;
		if ((e = read_at(fd[source], data, n, off, end)) ||
		    (e = read_at(fd[target], check, n, off, end)))
			goto out;
		if (memcmp(data, check, n)) {
			e = -EIO;
			goto out;
		}
		off += n;
	}
out:
	if (data) {
		explicit_bzero(data, IO_CHUNK);
		free(data);
	}
	if (check) {
		explicit_bzero(check, IO_CHUNK);
		free(check);
	}
	return e;
}

int nv_vendor_initial_timestamps(const int fix_fd[2], int runtime_fd, uint32_t *fix_value,
				 uint32_t *runtime_value, uint64_t deadline)
{
	uint8_t first_words[4], head[1024];
	int e = -EBADMSG;
	unsigned first = (rand() & 1) ? 0u : 1u;
	for (unsigned attempt = 0; attempt < 2; attempt++) {
		e = valid_stored_image(fix_fd[first ^ attempt], 7340032u, first_words, NULL, deadline);
		if (e == -ETIMEDOUT || e == -ENOMEM)
			return e;
		if (!e)
			break;
	}
	if (e)
		return e;
	e = read_at(runtime_fd, head, sizeof(head), 0, deadline);
	if (e)
		return e;
	*fix_value = get32(first_words);
	*runtime_value = get32(head + 516);
	return 0;
}
