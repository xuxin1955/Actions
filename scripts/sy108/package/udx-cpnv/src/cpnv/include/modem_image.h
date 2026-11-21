/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef UDX_MODEM_IMAGE_H
#define UDX_MODEM_IMAGE_H
#include <stddef.h>
#include <stdint.h>

#define MCI_HEADER_BYTES 132u
#define MCI_DESCRIPTOR_COUNT 10u
#define MCI_PREFIX_BYTES 512u
#define MCI_SUFFIX_BYTES 4096u

enum mci_format {
	MCI_RAW,
	MCI_SCI,
	MCI_SCI_SIGNED,
	MCI_SCI_PAYLOAD,
	MCI_DHTB
};

enum mci_result {
	MCI_OK = 0,
	MCI_E_ARGUMENT = -1,
	MCI_E_HEADER_SIZE = -2,
	MCI_E_MAGIC = -3,
	MCI_E_FLAGS = -4,
	MCI_E_DUPLICATE = -5,
	MCI_E_TERMINATOR = -6,
	MCI_E_NO_IMAGE = -7,
	MCI_E_RANGE = -8,
	MCI_E_EMPTY = -9
};

struct mci_extent {
	uint64_t source_offset;
	uint64_t target_offset;
	uint64_t length;
};

struct mci_plan {
	struct mci_extent extents[2];
	uint64_t total_bytes;
	uint32_t extent_count;
	uint32_t image_descriptor;
	uint32_t terminal_descriptor;
	uint32_t image_flags;
	uint32_t header_offset;
	/* Vendor ignores these bytes. Do not invent a zero/version requirement. */
	uint8_t opaque_header[8];
};

/* Metadata only. header must contain exactly 132 bytes, read independently at
 * source offset 0 or 512. The function never reads a file or image body.
 *
 * Mate50 AP code: modem_control 0x403b5c..0x403dc8 and 0x402b54..0x402c90.
 * SCI1 + 8 opaque bytes + at most ten little-endian {flags,offset,length}
 * descriptors; low flags byte 1 selects the modem image, bit 8 terminates the
 * table, and bit 10 strips 20 bytes from the selected extent. A header at 512
 * produces two ordered copies: the opaque first 512 bytes to target 0, followed
 * by selected offset+512/length+4096 to target 512. A header at zero produces
 * just the selected extent to target zero.
 *
 * Deliberate safety restrictions beyond vendor: reject unsupported flag bits,
 * duplicate type 1, missing terminator, malformed/overlapping-header ranges,
 * empty selected payload and any 32-bit arithmetic overflow. Nonselected
 * descriptors are bounds-checked but their data/types are not interpreted.
 * Bytes after the first terminator are unused, as in the reviewed AP loop.
 * No claim about cryptographic validity, approved firmware, CP compatibility,
 * source freshness or physical target layout follows from MCI_OK.
 * On error, a non-NULL output is zeroed and contains no usable partial plan.
 */
int mci_parse_sci_header(const uint8_t *header, size_t header_bytes, uint32_t header_offset,
			 uint64_t source_bytes, uint64_t target_capacity, struct mci_plan *out);

/* Native remoteproc consumes the selected SCI payload, without its outer
 * signing prefix or trailer. Bounds apply to the bytes actually copied. */
int mci_parse_sci_payload(const uint8_t *, size_t, uint32_t, uint64_t, uint64_t,
			  struct mci_plan *);

/* DHTB v1 declares its payload size at byte 48, after a 512-byte prefix.
 * Signing trailers and unused volume bytes are not part of the payload. */
int mci_parse_dhtb_header(const uint8_t *, size_t, uint64_t, uint64_t,
			  struct mci_plan *);

#endif
