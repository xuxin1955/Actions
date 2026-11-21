/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "capsule.h"
#include <errno.h>
#include <fcntl.h>
#include <libfdt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static int property(const void *dt, int node, const char *name, const char *value)
{
	int bytes;
	const char *p = fdt_getprop(dt, node, name, &bytes);
	return p && bytes == (int)strlen(value) + 1 && !memcmp(p, value, bytes);
}

static int size_arg(const char *text, unsigned long long *value)
{
	char *end;
	if (!*text || *text == '-')
		return -1;
	errno = 0;
	*value = strtoull(text, &end, 10);
	return errno || *end || !*value ? -1 : 0;
}

static uint64_t le64(const unsigned char *p)
{
	uint64_t n = 0;
	for (unsigned i = 0; i < 8; i++)
		n |= (uint64_t)p[i] << (8 * i);
	return n;
}

int main(int argc, char **argv)
{
	struct stat st;
	struct udx_boot_parts parts;
	int fd, result = 1;
	void *data;
	if (argc != 6)
		return 64;
	unsigned long long root_bytes, boot_capacity, root_capacity;
	if (size_arg(argv[3], &root_bytes) || size_arg(argv[4], &boot_capacity) ||
	    size_arg(argv[5], &root_capacity) || root_bytes < 96 || root_bytes > root_capacity)
		return 1;
	fd = open(argv[1], O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return 1;
	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
	    (uint64_t)st.st_size > boot_capacity || st.st_size > UDX_BOOT_VOLUME_BYTES) {
		close(fd);
		return 1;
	}
	size_t kernel_bytes = st.st_size;
	data = mmap(NULL, kernel_bytes, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (data == MAP_FAILED)
		return 1;
	if (udx_boot_parts(data, st.st_size, UDX_BOOT_CHOICE_OPENWRT, &parts) ||
	    fdt_check_header(parts.dtb) || fdt_node_check_compatible(parts.dtb, 0, "sprd,sy108"))
		goto done;
	int chosen = fdt_path_offset(parts.dtb, "/chosen"), bytes;
	if (chosen < 0 || !property(parts.dtb, chosen, "udx,boot-policy", "openwrt-v1") ||
	    !property(parts.dtb, chosen, "udx,boot-target", "openwrt") ||
	    !fdt_getprop(parts.dtb, chosen, "udx,trial-local-lease", &bytes))
		goto done;
	unsigned char header[96];
	fd = open(argv[2], O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		goto done;
	int bad = read(fd, header, sizeof(header)) != sizeof(header);
	close(fd);
	if (bad || memcmp(header, "hsqs", 4) || header[28] != 4 || header[29] ||
	    le64(header + 40) < sizeof(header) || le64(header + 40) > root_bytes)
		goto done;
	result = 0;
done:
	munmap(data, kernel_bytes);
	if (result)
		fputs("Unsupported SY108 kernel or rootfs image\n", stderr);
	return result;
}
