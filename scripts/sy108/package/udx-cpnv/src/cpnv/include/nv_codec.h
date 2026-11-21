/* SPDX-License-Identifier: MIT */
#ifndef NV_CODEC_H
#define NV_CODEC_H

#include <stddef.h>
#include <stdint.h>

/* NV frame and image checksum primitives. */
#define NV_HEADER 512u
#define NV_SECTOR 512u
#define NV_MAX_IMAGE (16u * 1024u * 1024u)
#define NV_MAX_PAYLOAD 262144u
#define NV_FRAME_HEADER 20u
#define NV_COMMAND_SIZE 20u
#define NV_RESPONSE_SIZE 36u
#define NV_FRAME_MAGIC UINT32_C(0x524d4e56)
#define NV_IMAGE_MAGIC UINT32_C(0x4e56)
#define NV_IMAGE_FORMAT UINT32_C(101)

struct nv_frame {
	uint32_t opaque;
	uint32_t group;
	uint32_t sub;
	uint32_t payload_size;
	const uint8_t *payload;
};

struct nv_command {
	uint32_t kind;
	uint32_t sequence;
	uint32_t arg;
	uint32_t sector;
	uint32_t count;
};

/*
 * All int-returning functions return 0 on success and -1 on invalid input.
 * Failed operations leave output buffers/objects unchanged. Buffers and output
 * objects must not overlap. No function allocates memory or retains pointers.
 * Every supplied non-null buffer must contain its stated number of bytes.
 */

/* Exact, already-delimited frame only; rejects trailing and coalesced data.
 * payload is a borrowed view into raw and remains valid only as long as raw.
 * Empty payloads are structurally valid; transaction policy belongs upstream.
 */
int nv_frame_decode(const uint8_t *raw, size_t len, struct nv_frame *out);

/* Canonical 36-byte response. status must be 0 or 1; ACK is not durability. */
int nv_response_encode(const struct nv_frame *frame, int status, uint8_t out[NV_RESPONSE_SIZE]);

/* Decode exactly 20 available bytes; kind/state/bounds checks belong upstream. */
int nv_command_decode(const uint8_t raw[NV_COMMAND_SIZE], struct nv_command *out);

/*
 * LE16 sum, optional low odd byte, accumulator wrap, exactly two observed
 * folds, complemented low 16 bits. bits must be 32 or 64; len <= NV_MAX_IMAGE.
 * NULL is permitted only for zero length. Invalid arguments return 0, which
 * is also a valid checksum: callers must validate arguments independently.
 */
uint64_t nv_checksum_sum(const uint8_t *, size_t);
uint16_t nv_checksum_finish(uint64_t, unsigned);
uint16_t nv_checksum(const uint8_t *data, size_t len, unsigned bits);

#endif
