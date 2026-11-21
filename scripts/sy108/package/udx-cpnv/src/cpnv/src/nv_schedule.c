/* SPDX-License-Identifier: GPL-2.0-only */
#include "nv_schedule.h"
#include <string.h>

void nvc_discard(struct nvc_schedule *s)
{
	if (s->backend)
		s->actions.discard(s->actions.context, s->backend);
	s->backend = 0;
	s->busy = 0;
	s->active = 0;
	s->header_used = 0;
	s->remaining = 0;
	s->offset = 0;
	s->sequence = 0;
	s->frames = 0;
	s->commands = 0;
	s->partition = 0;
	s->writes = 0;
	s->final_pending = 0;
}

void nvc_init(struct nvc_schedule *s, const struct nvc_actions *a)
{
	memset(s, 0, sizeof(*s));
	s->actions = *a;
	s->group = UINT32_MAX;
}

static int terminal(struct nvc_schedule *s, int error)
{
	s->terminal_error = error;
	return error;
}

int nvc_finish_final_reply(struct nvc_schedule *s, size_t actual_bytes, int transport_error)
{
	int result;
	if (!s)
		return NVU_STATE;
	if (s->terminal_error)
		return s->terminal_error;
	if (!s->final_pending || !s->backend)
		return NVU_STATE;
	result = nv_ubi_note_ack_result(s->backend, actual_bytes);
	if (transport_error) {
		nv_ubi_abort(s->backend);
		result = transport_error < 0 ? transport_error : NVU_REPLY_ERROR;
		s->backend->last_error = result;
	}
	if (result)
		return terminal(s, result);
	s->final_pending = 0;
	return 0;
}

static int nack(struct nvc_schedule *s)
{
	nvc_discard(s);
	return NVC_NACK;
}

int nvc_frame(struct nvc_schedule *s, const uint8_t *raw, size_t length)
{
	struct nv_frame f;
	const uint8_t *p;
	size_t n;
	if (!s)
		return NVU_PROTOCOL;
	if (s->terminal_error)
		return s->terminal_error;
	if (s->final_pending)
		return NVU_STATE;
	if (nv_frame_decode(raw, length, &f))
		return terminal(s, NVU_PROTOCOL);
	/* Stock getPacket: invalid continuation is skipped without changing state. */
	if (f.group == s->group) {
		if (f.sub != (uint32_t)(s->sub + 1u))
			return NVC_SILENT;
		s->sub = f.sub;
		if (!s->busy)
			return NVC_NACK;
	} else {
		if (f.sub)
			return NVC_SILENT;
		nvc_discard(s);
		s->group = f.group;
		s->sub = 0;
		s->busy = 1;
	}
	p = f.payload;
	n = f.payload_size;
	while (n) {
		size_t take;
		if (s->remaining) {
			int r;
			take = n < s->remaining ? n : s->remaining;
			r = nv_ubi_stage(s->backend, s->offset, p, take);
			if (r)
				return terminal(s, r);
			s->offset += (uint32_t)take;
			s->remaining -= (uint32_t)take;
			p += take;
			n -= take;
			if (s->remaining)
				return NVC_PROGRESS;
			continue; /* exact extent exhaustion does NOT ACK */
		}
		take = NV_COMMAND_SIZE - s->header_used;
		if (take > n)
			take = n;
		memcpy(s->header + s->header_used, p, take);
		s->header_used += (unsigned)take;
		p += take;
		n -= take;
		if (s->header_used < NV_COMMAND_SIZE)
			return NVC_PROGRESS;
		{
			struct nv_command c;
			uint64_t off, len;
			int r;
			s->header_used = 0;
			if (nv_command_decode(s->header, &c))
				return nack(s);
			if (!s->active) {
				if (c.kind || (c.arg != 1 && c.arg != 2))
					return nack(s);
				r = s->actions.begin(s->actions.context, c.arg, &s->backend);
				if (r)
					return terminal(s, r);
				s->partition = c.arg;
				s->sequence = c.sequence;
				s->active = 1;
				continue;
			}
			if (c.sequence != (uint32_t)(s->sequence + 1u))
				return nack(s);
			s->sequence = c.sequence;
			if (c.kind == 1) {
				off = (uint64_t)c.sector * NV_SECTOR;
				len = (uint64_t)c.count * NV_SECTOR;
				if (!len || off > s->backend->payload_size ||
				    len > s->backend->payload_size - off)
					return nack(s);
				s->offset = (uint32_t)off;
				s->remaining = (uint32_t)len;
				s->writes++;
			} else if (c.kind == 2) {
				/* OEM END completes the group and discards remaining outer-frame
				 * bytes. */
				if (s->writes && s->actions.commit_state &&
				    (r = s->actions.commit_state(s->actions.context, 1)))
					return terminal(s, r);
				r = s->writes ? nv_ubi_commit(s->backend, f.group, f.sub)
					      : nvc_verify_noop(s->backend, f.group, f.sub);
				if (s->writes && s->actions.commit_state) {
					int event_error =
					    s->actions.commit_state(s->actions.context, 0);
					if (!r)
						r = event_error;
				}
				if (r)
					return terminal(s, r);
				s->busy = 0;
				s->active = 0;
				s->final_pending = 1;
				return NVC_FINAL;
			} else
				return nack(s);
		}
	}
	return NVC_SILENT;
}
