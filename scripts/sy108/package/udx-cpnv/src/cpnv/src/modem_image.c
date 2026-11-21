/* SPDX-License-Identifier: GPL-2.0-only */
#include "modem_image.h"

#define MCI_LAST UINT32_C(0x100)
#define MCI_SKIP20 UINT32_C(0x400)
#define MCI_KNOWN_FLAGS UINT32_C(0x7ff) /* stock NR descriptor uses 0x301 */

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void clear(void *p, size_t n)
{
	uint8_t *bytes = p;
	while (n--)
		*bytes++ = 0;
}

static int parse_sci(const uint8_t *header, size_t header_bytes, uint32_t header_offset,
		     uint64_t source_bytes, uint64_t target_capacity, struct mci_plan *out,
		     int envelope)
{
	struct mci_plan plan;
	uint32_t selected_offset = 0, selected_length = 0;
	unsigned selected = 0, terminated = 0, i;

	if (out)
		clear(out, sizeof(*out));
	if (!header || !out || (header_offset != 0 && header_offset != MCI_PREFIX_BYTES))
		return MCI_E_ARGUMENT;
	if (header_bytes != MCI_HEADER_BYTES)
		return MCI_E_HEADER_SIZE;
	if (header_offset > source_bytes || MCI_HEADER_BYTES > source_bytes - header_offset)
		return MCI_E_RANGE;
	if (header[0] != 'S' || header[1] != 'C' || header[2] != 'I' || header[3] != '1')
		return MCI_E_MAGIC;
	clear(&plan, sizeof(plan));
	plan.header_offset = header_offset;
	for (i = 0; i < sizeof(plan.opaque_header); ++i)
		plan.opaque_header[i] = header[4 + i];

	for (i = 0; i < MCI_DESCRIPTOR_COUNT; ++i) {
		const uint8_t *descriptor = header + 12 + i * 12;
		uint32_t flags = get32(descriptor);
		uint32_t offset = get32(descriptor + 4);
		uint32_t length = get32(descriptor + 8);
		uint64_t end = (uint64_t)offset + length;

		if (flags & ~MCI_KNOWN_FLAGS)
			return MCI_E_FLAGS;
		/* Vendor uses a wrapping u32 sum and rejects only UINT32_MAX at the
		 * terminal record. Reject all overflows and that observed sentinel. */
		if (end > UINT32_MAX || ((flags & MCI_LAST) && end == UINT32_MAX))
			return MCI_E_RANGE;
		if (end > source_bytes - header_offset || (length && offset < MCI_HEADER_BYTES))
			return MCI_E_RANGE;
		if ((flags & UINT32_C(0xff)) == 1) {
			if (selected)
				return MCI_E_DUPLICATE;
			if (!length)
				return MCI_E_EMPTY;
			selected = 1;
			selected_offset = offset;
			selected_length = length;
			if (flags & MCI_SKIP20) {
				if (length <= 20 || offset > UINT32_MAX - 20)
					return MCI_E_RANGE;
				selected_offset += 20;
				selected_length -= 20;
			}
			plan.image_descriptor = i;
			plan.image_flags = flags;
		}
		if (flags & MCI_LAST) {
			terminated = 1;
			plan.terminal_descriptor = i;
			break;
		}
	}
	if (!terminated)
		return MCI_E_TERMINATOR;
	if (!selected)
		return MCI_E_NO_IMAGE;

	if (header_offset && envelope) {
		if (selected_offset > UINT32_MAX - MCI_PREFIX_BYTES ||
		    selected_length > UINT32_MAX - MCI_SUFFIX_BYTES)
			return MCI_E_RANGE;
		selected_offset += MCI_PREFIX_BYTES;
		selected_length += MCI_SUFFIX_BYTES;
		plan.extent_count = 2;
		plan.extents[0].length = MCI_PREFIX_BYTES;
		plan.extents[1].source_offset = selected_offset;
		plan.extents[1].target_offset = MCI_PREFIX_BYTES;
		plan.extents[1].length = selected_length;
	} else {
		plan.extent_count = 1;
		plan.extents[0].source_offset = (uint64_t)header_offset + selected_offset;
		plan.extents[0].length = selected_length;
	}
	for (i = 0; i < plan.extent_count; ++i) {
		const struct mci_extent *extent = &plan.extents[i];
		if (extent->source_offset > source_bytes ||
		    extent->length > source_bytes - extent->source_offset ||
		    extent->target_offset > target_capacity ||
		    extent->length > target_capacity - extent->target_offset)
			return MCI_E_RANGE;
		if (plan.total_bytes > UINT64_MAX - extent->length)
			return MCI_E_RANGE;
		plan.total_bytes += extent->length;
	}
	*out = plan;
	return MCI_OK;
}

int mci_parse_sci_header(const uint8_t *header, size_t header_bytes, uint32_t header_offset,
			 uint64_t source_bytes, uint64_t target_capacity, struct mci_plan *out)
{
	return parse_sci(header, header_bytes, header_offset, source_bytes, target_capacity, out, 1);
}

int mci_parse_sci_payload(const uint8_t *header, size_t header_bytes, uint32_t header_offset,
			  uint64_t source_bytes, uint64_t target_capacity, struct mci_plan *out)
{
	return parse_sci(header, header_bytes, header_offset, source_bytes, target_capacity, out, 0);
}

int mci_parse_dhtb_header(const uint8_t *header, size_t header_bytes, uint64_t source_bytes,
			  uint64_t target_capacity, struct mci_plan *out)
{
	if (out)
		clear(out, sizeof(*out));
	if (!header || !out)
		return MCI_E_ARGUMENT;
	if (header_bytes < 52)
		return MCI_E_HEADER_SIZE;
	if (get32(header) != UINT32_C(0x42544844) || get32(header + 4) != 1)
		return MCI_E_MAGIC;
	uint32_t length = get32(header + 48);
	if (!length)
		return MCI_E_EMPTY;
	if (source_bytes < MCI_PREFIX_BYTES || length > source_bytes - MCI_PREFIX_BYTES ||
	    length > target_capacity)
		return MCI_E_RANGE;
	out->extent_count = 1;
	out->extents[0] = (struct mci_extent){MCI_PREFIX_BYTES, 0, length};
	out->total_bytes = length;
	return MCI_OK;
}
