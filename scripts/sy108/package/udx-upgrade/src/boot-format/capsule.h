/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef UDX_OPENWRT_CAPSULE_H
#define UDX_OPENWRT_CAPSULE_H
#include "misc-state.h"
#define UDX_CAPSULE_HEADER_OFFSET 128u
#define UDX_CAPSULE_PAYLOAD_OFFSET 4096u
#define UDX_CAPSULE_MAGIC 0x315a4758u /* XGZ1, fixed little-endian target ABI */
#define UDX_IMAGE_SOURCE 0x80080000ul
#define UDX_IMAGE_DESTINATION 0x80200000ul
#define UDX_IMAGE_CACHE 0x82000000ul
#define UDX_FDT_ADDRESS 0x85400000ul
#define UDX_INITRD_ADDRESS 0x85500000ul
#define UDX_IMAGE_LIMIT (UDX_IMAGE_CACHE - UDX_IMAGE_DESTINATION)

struct udx_capsule {
	uint32_t magic, version, target, gzip_size;
	uint32_t image_size, image_memory_size, gzip_crc, image_crc;
	uint32_t ramdisk_size, ramdisk_crc, dtb_size, dtb_crc;
	uint32_t header_crc, reserved[3];
};

_Static_assert(sizeof(struct udx_capsule) == 64, "capsule metadata ABI");

struct udx_boot_parts {
	const unsigned char *kernel, *ramdisk, *dtb, *gzip;
	struct udx_capsule capsule;
	uint32_t kernel_size, ramdisk_size, dtb_size, total;
};

int udx_boot_parts(const void *, size_t, unsigned target, struct udx_boot_parts *);
#endif
