/* SPDX-License-Identifier: GPL-2.0-only
 * Strict fixed-format parser shared by user-space image validation.
 * Rejects oversized headers before accessing payloads or inflating data.
 */
#include "capsule.h"

static uint32_t le32(const unsigned char *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t be32(const unsigned char *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint64_t le64(const unsigned char *p)
{
	return le32(p) | ((uint64_t)le32(p + 4) << 32);
}

static int bytes(const unsigned char *p, const char *s, unsigned n)
{
	for (unsigned i = 0; i < n; i++)
		if (p[i] != (unsigned char)s[i])
			return 0;
	return 1;
}

static uint32_t align(uint32_t n)
{
	return (n + 2047u) & ~2047u;
}

int udx_boot_parts(const void *data, size_t size, unsigned target, struct udx_boot_parts *out)
{
	const unsigned char *p = data;
	struct udx_boot_parts v = {0};
	uint32_t capacity =
	    target == UDX_BOOT_CHOICE_OPENWRT ? UDX_BOOT_VOLUME_BYTES : UDX_RECOVERY_VOLUME_BYTES;
	if (!p || !out || (target != 1 && target != 2) || size < 2560 + 4096 || size > capacity)
		return -1;
	if (!bytes(p, "DHTB\1\0\0\0", 8) || le32(p + 48) != size - 512 ||
	    !bytes(p + 512, "ANDROID!", 8))
		return -2;
	uint32_t k = le32(p + 520), r = le32(p + 528), d = le32(p + 552);
	if (le32(p + 548) != 2048 || le32(p + 536) || k <= 4096 || k > capacity || !r ||
	    r > capacity || d < 2048 || d > capacity)
		return -3;
	uint64_t rd = 2560ull + align(k), dt = rd + align(r), end = dt + d;
	if (end != size || dt + 2048 > size)
		return -4;
	const unsigned char *sp = p + dt;
	if (!bytes(sp, "SPRD", 4) || le32(sp + 4) != 1 || le32(sp + 8) != 1 ||
	    le32(sp + 12) != 710 || le32(sp + 16) != 1 || le32(sp + 20) != 0x20000 ||
	    le32(sp + 24) != 2048 || le32(sp + 28) != d - 2048)
		return -5;
	v.kernel = p + 2560;
	v.ramdisk = p + rd;
	v.dtb = sp + 2048;
	v.gzip = v.kernel + 4096;
	v.kernel_size = k;
	v.ramdisk_size = r;
	v.total = (uint32_t)size;
	if (!bytes(v.kernel + 56, "ARMd", 4) || le64(v.kernel + 8) != 0x80000 ||
	    le64(v.kernel + 24) != 0xa)
		return -6;
	unsigned char *copy = (unsigned char *)&v.capsule;
	for (unsigned i = 0; i < sizeof(v.capsule); i++)
		copy[i] = v.kernel[UDX_CAPSULE_HEADER_OFFSET + i];
	struct udx_capsule *h = &v.capsule;
	if (h->magic != UDX_CAPSULE_MAGIC || h->version != 1 || h->target != target ||
	    h->reserved[0] || h->reserved[1] || h->reserved[2] || udx_crc32(h, 48) != h->header_crc)
		return -7;
	if (h->gzip_size != k - 4096 || h->gzip_size < 18 || h->image_size < 64 ||
	    h->image_size > UDX_IMAGE_LIMIT || h->image_memory_size < h->image_size ||
	    h->image_memory_size > UDX_IMAGE_LIMIT ||
	    le64(v.kernel + 16) != UDX_IMAGE_DESTINATION + h->image_memory_size - UDX_IMAGE_SOURCE)
		return -8;
	if (h->ramdisk_size != r || h->dtb_size < 40 || h->dtb_size > d - 2048 ||
	    be32(v.dtb) != 0xd00dfeed || be32(v.dtb + 4) != h->dtb_size || be32(v.dtb + 20) != 17)
		return -9;
	v.dtb_size = h->dtb_size;
	/* Standard gzip header only: no unchecked optional name/comment scanning in
	 * the U-Boot gunzip helper. ISIZE and CRC are checked again after decoding.
	 */
	if (!bytes(v.gzip, "\37\213\10\0", 4) || le32(v.gzip + h->gzip_size - 4) != h->image_size ||
	    le32(v.gzip + h->gzip_size - 8) != h->image_crc ||
	    udx_crc32(v.gzip, h->gzip_size) != h->gzip_crc ||
	    udx_crc32(v.ramdisk, r) != h->ramdisk_crc ||
	    udx_crc32(v.dtb, h->dtb_size) != h->dtb_crc)
		return -10;
	*out = v;
	return 0;
}
