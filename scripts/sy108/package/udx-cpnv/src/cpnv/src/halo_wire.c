/* SPDX-License-Identifier: GPL-2.0-only */
#include "halo.h"
#include <errno.h>

uint8_t halo_reverse(uint8_t x)
{
	x = (uint8_t)((x >> 4) | (x << 4));
	x = (uint8_t)(((x << 2) & 0xcc) | ((x >> 2) & 0x33));
	return (uint8_t)(((x << 1) & 0xaa) | ((x >> 1) & 0x55));
}

/* Preserve the token byte scrambling used by the CP wire protocol. */
static uint8_t transform(uint8_t x, const uint8_t token[4])
{
	unsigned i;
	for (i = 0; i < 4; i++) {
		uint8_t k = token[i];
		x = (uint8_t)(halo_reverse(
				  (uint8_t)(halo_reverse((uint8_t)(x ^ k)) ^ halo_reverse(k))) ^
			      k);
		x = halo_reverse(x);
	}
	return x;
}

void halo_token_encode(const uint8_t t[4], const uint8_t n[2], uint8_t out[4])
{
	uint8_t a = n[0], b = n[1];
	out[2] = transform(a, t);
	out[3] = transform(b, t);
	out[0] = a;
	out[1] = b;
}

int halo_token_check(const uint8_t t[4], const uint8_t p[4])
{
	unsigned diff = (unsigned)(transform(p[0], t) ^ p[2]);
	diff |= (unsigned)(transform(p[1], t) ^ p[3]);
	return diff == 0;
}

void halo_factory_query_frame(const uint8_t token[4], const uint8_t nonce[2], uint8_t tail,
			      uint8_t out[9])
{
	out[0] = 0xfc;
	out[1] = 0xab;
	out[2] = 0xa1;
	out[3] = 0;
	halo_token_encode(token, nonce, out + 4);
	out[8] = tail;
}
