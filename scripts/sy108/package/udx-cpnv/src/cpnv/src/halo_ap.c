/* SPDX-License-Identifier: GPL-2.0-only */
#include "halo.h"

void halo_ap_query_frame(uint8_t out[16])
{
	unsigned i;
	for (i = 0; i < 16; i++)
		out[i] = 0;
	out[0] = 0xfc;
	out[1] = 0xab;
	out[2] = 0xa3; /* [9]=0 query, not mark-initialized */
}

void halo_ap_mark_frame(uint8_t out[16])
{
	halo_ap_query_frame(out);
	out[9] = 1;
}

int halo_ap_state_decode(const uint8_t *p, size_t n, unsigned *out)
{
	if (!p || !out || n != 16)
		return -22;
	*out = p[11];
	return 0;
}
