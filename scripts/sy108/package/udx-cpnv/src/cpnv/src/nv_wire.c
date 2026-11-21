/* SPDX-License-Identifier: GPL-2.0-only */
#include "nv_wire.h"
#include <errno.h>
#define NVC_WIRE_PROTOCOL (-20)

static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

int nvc_wire_read(const struct nvc_wire_io *io, uint8_t *packet, size_t capacity,
		  struct nv_frame *frame)
{
	if (!io || !io->read_exact || !packet || !frame ||
	    capacity < NV_FRAME_HEADER + NV_MAX_PAYLOAD)
		return -EINVAL;
	int error = io->read_exact(io->context, packet, NV_FRAME_HEADER, 1);
	if (error)
		return error;
	uint32_t payload = le32(packet + 16);
	if (le32(packet) != NV_FRAME_MAGIC || payload > NV_MAX_PAYLOAD)
		return NVC_WIRE_PROTOCOL;
	/* Consume the entire slot before committing/ACK. Padding is opaque and is
	 * discarded, just as the stock full-record read does; never scan for magic. */
	error = io->read_exact(io->context, packet + NV_FRAME_HEADER, NV_MAX_PAYLOAD, 0);
	if (error)
		return error;
	return nv_frame_decode(packet, NV_FRAME_HEADER + (size_t)payload, frame) ? NVC_WIRE_PROTOCOL
										 : 0;
}
