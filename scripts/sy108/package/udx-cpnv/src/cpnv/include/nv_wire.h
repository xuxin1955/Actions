/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef UDX_NV_WIRE_H
#define UDX_NV_WIRE_H
#include "nv_codec.h"

/* Target nvpacket_size is the physical payload slot, not the valid byte count
 * at header+16. Every CP record occupies 20+0x40000 bytes in the SBUF stream. */
struct nvc_wire_io {
	void *context;
	int (*read_exact)(void *, uint8_t *, size_t, int first);
};

int nvc_wire_read(const struct nvc_wire_io *, uint8_t *, size_t, struct nv_frame *);
#endif
