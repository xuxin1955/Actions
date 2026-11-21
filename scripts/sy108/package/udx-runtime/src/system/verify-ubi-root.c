/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "root-health.h"

int main(int argc, char **argv)
{
	struct udx_root_layout layout;
	int preinit, procd;
	if (argc != 2)
		return 64;
	if (!strcmp(argv[1], "preinit")) {
		preinit = 1;
		procd = 0;
	} else if (!strcmp(argv[1], "mounted")) {
		preinit = 0;
		procd = 0;
	} else if (!strcmp(argv[1], "running")) {
		preinit = 0;
		procd = 1;
	} else
		return 64;
	int ret = udx_root_check(preinit, procd, &layout);
	if (ret) {
		fprintf(stderr, "udx-root-check: %s failed: %s (%d)\n", argv[1], strerror(-ret),
			ret);
		return 1;
	}
	printf("ubipac=mtd%u rootfs=ubi%u_%u rootfs_data=ubi%u_%u phase=%s\n", layout.mtd,
	       layout.ubi, layout.rootfs, layout.ubi, layout.data, argv[1]);
	return 0;
}
