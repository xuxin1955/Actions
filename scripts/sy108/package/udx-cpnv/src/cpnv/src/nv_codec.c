/* SPDX-License-Identifier: MIT */
#include "nv_codec.h"

/* Byte accesses avoid alignment, aliasing, and host-endian assumptions. */
static uint32_t nv_load32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

static void nv_store32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

int nv_frame_decode(const uint8_t *raw, size_t len, struct nv_frame *out)
{
	uint32_t size;

	if (raw == NULL || out == NULL || len < NV_FRAME_HEADER)
		return -1;
	if (nv_load32(raw) != NV_FRAME_MAGIC)
		return -1;
	size = nv_load32(raw + 16);
	/* Subtract after checking len, avoiding size + 20 overflow altogether. */
	if (size > NV_MAX_PAYLOAD || len - NV_FRAME_HEADER != (size_t)size)
		return -1;
	out->opaque = nv_load32(raw + 4);
	out->group = nv_load32(raw + 8);
	out->sub = nv_load32(raw + 12);
	out->payload_size = size;
	out->payload = raw + NV_FRAME_HEADER;
	return 0;
}

int nv_response_encode(const struct nv_frame *frame, int status, uint8_t out[NV_RESPONSE_SIZE])
{
	size_t i;

	if (frame == NULL || out == NULL || (status != 0 && status != 1))
		return -1;
	for (i = 0; i < NV_RESPONSE_SIZE; ++i)
		out[i] = 0;
	nv_store32(out, NV_FRAME_MAGIC);
	nv_store32(out + 8, frame->group);
	nv_store32(out + 12, frame->sub);
	nv_store32(out + 16, 1);
	out[20] = (uint8_t)status;
	return 0;
}

int nv_command_decode(const uint8_t raw[NV_COMMAND_SIZE], struct nv_command *out)
{
	if (raw == NULL || out == NULL)
		return -1;
	out->kind = nv_load32(raw);
	out->sequence = nv_load32(raw + 4);
	out->arg = nv_load32(raw + 8);
	out->sector = nv_load32(raw + 12);
	out->count = nv_load32(raw + 16);
	return 0;
}

uint64_t nv_checksum_sum(const uint8_t *data, size_t length)
{
	uint64_t sum = 0;
	size_t i;
	for (i = 0; i + 1 < length; i += 2)
		sum += (uint64_t)data[i] | ((uint64_t)data[i + 1] << 8);
	if (i < length)
		sum += data[i];
	return sum;
}

uint16_t nv_checksum_finish(uint64_t sum, unsigned bits)
{
	if (bits != 32 && bits != 64)
		return 0;
	/* The OEM accumulator wraps at the selected width before exactly two folds. */
	if (bits == 32)
		sum = (uint32_t)sum;
	sum = (sum & UINT64_C(0xffff)) + (sum >> 16);
	sum += sum >> 16;
	return (uint16_t)~sum;
}

uint16_t nv_checksum(const uint8_t *data, size_t length, unsigned bits)
{
	if (length > NV_MAX_IMAGE || (!data && length))
		return 0;
	return nv_checksum_finish(nv_checksum_sum(data, length), bits);
}
