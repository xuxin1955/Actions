/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef CPNV_HALO_H
#define CPNV_HALO_H
#include <stddef.h>
#include <stdint.h>
/* Bytes 9..92 contain the feature record. Consume all 152 wire bytes,
 * including the opaque tail, before reading the next HALO response. */
#define HALO_FEATURE_REPLY_BYTES 152u
int halo_nonce_digest(const uint8_t[16], uint8_t[20]);
int halo_feature_digest(const uint8_t[64], uint8_t[20]);
int halo_material_block(const uint8_t[16], uint32_t, uint8_t[128]);
int halo_rsa1024_public(const uint8_t[128], const uint8_t[128], uint8_t[128]);
int halo_material_verify(const uint8_t[144], const uint8_t[128], uint32_t *);
uint8_t halo_reverse(uint8_t);
void halo_token_encode(const uint8_t[4], const uint8_t[2], uint8_t[4]);
int halo_token_check(const uint8_t[4], const uint8_t[4]);
void halo_factory_query_frame(const uint8_t[4], const uint8_t[2], uint8_t, uint8_t[9]);
void halo_feature_query_frame(const uint8_t[4], const uint8_t[2], uint8_t, uint8_t[9]);
int halo_feature_decode(const uint8_t *, size_t, const uint8_t[8], uint64_t *, unsigned *);
void halo_ap_query_frame(uint8_t[16]);
void halo_ap_mark_frame(uint8_t[16]);
int halo_ap_state_decode(const uint8_t *, size_t, unsigned *);
int halo_sign_material(const char *, const uint8_t[16], uint32_t, uint8_t[128]);
#endif
