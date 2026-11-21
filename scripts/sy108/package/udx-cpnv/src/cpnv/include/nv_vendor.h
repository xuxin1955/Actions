/* SPDX-License-Identifier: GPL-2.0-only */
/* UDX stock NV semantics, no sidecar format. */
#ifndef UDX_NV_VENDOR_H
#define UDX_NV_VENDOR_H
#include <stddef.h>
#include <stdint.h>
/* The logical RAM-disk capacity is stable; the stored header may be shorter. */
uint32_t nv_vendor_payload_size(const uint8_t header[512], uint32_t capacity);
int nv_vendor_valid(const uint8_t *, size_t, uint32_t, unsigned *);
/* Explicit boot/recovery only, CP stopped and caller owns both UBI volumes.
 * Repairs ONLY an invalid replica from the valid one, using the validated source image length.
 * Both-valid replicas remain unchanged even when their contents differ. */
struct nv_ubi_backend;
int nvc_verify_noop(struct nv_ubi_backend *, uint32_t, uint32_t);
struct nv_store;
void nv_vendor_runtime_timestamp(struct nv_store *, uint32_t, uint32_t);
int nv_vendor_initial_timestamps(const int fix_fd[2], int runtime_fd, uint32_t *, uint32_t *,
				 uint64_t);
int nv_vendor_repair_pair(const int fd[2], uint32_t payload, uint64_t deadline);
#endif
