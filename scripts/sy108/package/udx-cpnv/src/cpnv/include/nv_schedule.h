/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NV_SCHEDULE_H
#define NV_SCHEDULE_H
#include "nv_ubi_backend.h"
#ifndef NVU_PROTOCOL
#define NVU_PROTOCOL (-20)
#endif
/* Exact AP schedule, one active partition. Callbacks are local storage actions,
 * never default-success ownership witnesses. No transport I/O in this core. */
struct nvc_actions {
	void *context;
	int (*begin)(void *, uint32_t, struct nv_ubi_backend **);
	void (*discard)(void *, struct nv_ubi_backend *);
	int (*commit_state)(void *, int active);
};

enum nvc_reply { NVC_SILENT = 0, NVC_PROGRESS = 1, NVC_FINAL = 2, NVC_NACK = 3 };

struct nvc_schedule {
	struct nvc_actions actions;
	struct nv_ubi_backend *backend;
	uint32_t group, sub, sequence, remaining, offset, frames, commands, partition;
	unsigned busy, active, header_used, writes, final_pending;
	int terminal_error;
	uint8_t header[NV_COMMAND_SIZE];
};

int nvc_verify_noop(struct nv_ubi_backend *, uint32_t, uint32_t);
void nvc_init(struct nvc_schedule *, const struct nvc_actions *);
/* Final returns only after durable commit. No further frame is accepted until
 * nvc_finish_final_reply records the ACK outcome. A negative storage result is
 * retained until nvc_init starts a new service; discard does not clear it.
 * Protocol NACK discards staging and permits a later distinct group/sub0. */
int nvc_frame(struct nvc_schedule *, const uint8_t *, size_t);
/* ACK must first be prepared by nv_ubi_prepare_final_ack. actual_bytes are
 * bytes accepted locally, not proof of peer receipt. transport_error is zero
 * or the exact negative transport/epoch error, including after a full write. */
int nvc_finish_final_reply(struct nvc_schedule *, size_t actual_bytes, int transport_error);
void nvc_discard(struct nvc_schedule *);
#endif
