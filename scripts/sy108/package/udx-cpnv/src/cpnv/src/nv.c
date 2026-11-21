/* SPDX-License-Identifier: GPL-2.0-only */
#include "cpnv.h"
#include "nv_schedule.h"
#include "nv_wire.h"
#include "nv_vendor.h"
#include "nv_ubi_linux.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct nv_service {
	struct nv_store *store;
	struct nv_worker *worker;
	struct nv_ubi_backend backend;
	struct nv_ubi_linux_context linux;
	struct nvc_schedule schedule;
	uint64_t deadline;
	bool transaction, finishing, cancelled;
};

static int storage_live(void *arg)
{
	struct nv_service *s = arg;
	if (!s->finishing && worker_stopping(s->worker)) {
		s->cancelled = true;
		return -ECANCELED;
	}
	struct pollfd p = {s->worker->nv_fd, 0, 0};
	int n = poll(&p, 1, 0);
	return n < 0 ? -errno : (p.revents & (POLLERR | POLLHUP | POLLNVAL)) ? -EPIPE : 0;
}

static int wait_io(struct nv_service *s, short events, bool idle)
{
	for (;;) {
		int e = storage_live(s);
		if (e)
			return e;
		uint64_t now = cpnv_now();
		if (!idle && now >= s->deadline)
			return -ETIMEDOUT;
		struct pollfd fds[] = {{s->worker->nv_fd, events, 0},
				       {s->finishing ? -1 : s->worker->cancel, POLLIN, 0}};
		int ms = idle ? 1000 : (int)(s->deadline - now > 1000 ? 1000 : s->deadline - now);
		int n = poll(fds, 2, ms);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL))
			return -EPIPE;
		if ((e = storage_live(s)))
			return e;
		if (fds[0].revents & events)
			return 0;
	}
}

static int read_exact(void *arg, uint8_t *p, size_t length, int first)
{
	struct nv_service *s = arg;
	while (length) {
		bool idle = first && !s->transaction;
		int e = wait_io(s, POLLIN, idle);
		if (e)
			return e;
		if (idle) {
			s->deadline = cpnv_now() + s->worker->policy.nv.timeout_ms;
			s->transaction = true;
		}
		ssize_t n = read(s->worker->nv_fd, p, length);
		if (n < 0 && (errno == EINTR || errno == EAGAIN))
			continue;
		if (n <= 0)
			return n < 0 ? -errno : -EPIPE;
		p += n;
		length -= (size_t)n;
		first = 0;
	}
	return storage_live(s);
}

static size_t write_exact(struct nv_service *s, const uint8_t *p, size_t length, int *error)
{
	size_t done = 0;
	*error = 0;
	while (done < length) {
		if ((*error = wait_io(s, POLLOUT, false)))
			break;
		ssize_t n = write(s->worker->nv_fd, p + done, length - done);
		if (n < 0 && (errno == EINTR || errno == EAGAIN))
			continue;
		if (n <= 0) {
			*error = n < 0 ? -errno : -EPIPE;
			break;
		}
		done += (size_t)n;
	}
	if (!*error)
		*error = storage_live(s);
	return done;
}

static int begin(void *arg, uint32_t partition, struct nv_ubi_backend **out)
{
	struct nv_service *s = arg;
	if (partition < 1 || partition > 2)
		return NVU_CONFIG;
	unsigned at = (partition - 1) * 2;
	struct nv_worker *w = s->worker;
	struct nv_ubi_identity identity[] = {w->volumes[at].identity, w->volumes[at + 1].identity};
	struct nv_ubi_ops ops = nv_ubi_linux_ops(&s->linux);
	int e = nv_ubi_bind(&s->backend, s->store, &ops, w->volumes[at].fd, w->volumes[at + 1].fd,
			    identity, partition == 1 ? 7340032 : 9437184,
			    (int)w->policy.nv.write_policy, s->deadline);
	*out = &s->backend;
	return e ? e : nv_ubi_begin(&s->backend);
}

static void discard(void *arg, struct nv_ubi_backend *backend)
{
	(void)arg;
	if (backend->state == NVU_STAGING)
		nv_ubi_abort(backend);
	nv_ubi_forget(backend);
}

static int commit(void *arg, int active)
{
	struct nv_service *s = arg;
	struct nv_worker *w = s->worker;
	pthread_mutex_lock(&w->mutex);
	if (active && w->stop_requested) {
		pthread_mutex_unlock(&w->mutex);
		return -ECANCELED;
	}
	if (active)
		s->finishing = true;
	w->status.committing = !!active;
	pthread_mutex_unlock(&w->mutex);
	uint64_t one = 1;
	ssize_t n = write(w->event, &one, sizeof one);
	(void)n;
	return 0;
}

static int open_transport(struct nv_worker *worker)
{
	for (;;) {
		if (worker_stopping(worker))
			return -ECANCELED;
		if (cpnv_now() >= worker->deadline)
			return -ETIMEDOUT;
		int fd = cpnv_port_open("/dev/snv_lte", O_RDWR);
		if (fd >= 0 || !cpnv_port_pending(fd))
			return fd;
		struct pollfd cancel = {worker->cancel, POLLIN, 0};
		if (poll(&cancel, 1, 25) < 0 && errno != EINTR)
			return -errno;
	}
}

int nv_service_run(struct nv_worker *w)
{
	struct nv_service s = {.worker = w};
	s.linux = (struct nv_ubi_linux_context){&s, storage_live};
	w->nv_fd = open_transport(w);
	if (w->nv_fd < 0)
		return w->nv_fd;
	uint32_t fixed, runtime;
	int fds[] = {w->volumes[0].fd, w->volumes[1].fd};
	int e = nv_vendor_initial_timestamps(fds, w->volumes[2].fd, &fixed, &runtime,
					     cpnv_now() + 60000);
	if (e)
		return e;
	s.store = nv_store_create();
	if (!s.store)
		return -ENOMEM;
	nv_vendor_runtime_timestamp(s.store, fixed, runtime);
	struct worker_status state;
	worker_snapshot(w, &state);
	state.nv_ready = true;
	worker_publish(w, &state);
	struct nvc_actions actions = {&s, begin, discard, commit};
	nvc_init(&s.schedule, &actions);
	struct nvc_wire_io wire = {&s, read_exact};
	uint8_t *packet = malloc(NV_FRAME_HEADER + NV_MAX_PAYLOAD);
	if (!packet) {
		nv_store_destroy(s.store);
		return -ENOMEM;
	}
	for (;;) {
		struct nv_frame frame;
		uint8_t ack[NV_RESPONSE_SIZE];
		int write_error = 0;
		e = nvc_wire_read(&wire, packet, NV_FRAME_HEADER + NV_MAX_PAYLOAD, &frame);
		if (e)
			break;
		if (frame.group != s.schedule.group && !frame.sub && s.schedule.busy)
			s.deadline = cpnv_now() + w->policy.nv.timeout_ms;
		int reply =
		    nvc_frame(&s.schedule, packet, (size_t)frame.payload_size + NV_FRAME_HEADER);
		if (reply < 0) {
			e = reply;
			break;
		}
		if (reply == NVC_FINAL) {
			/* END has completed storage. A lost reply must not be reported as
			 * discarded, uncommitted NV data. */
			s.transaction = false;
			if ((e = nv_ubi_prepare_final_ack(&s.backend, ack)))
				break;
			size_t sent = write_exact(&s, ack, sizeof ack, &write_error);
			e = nvc_finish_final_reply(&s.schedule, sent, write_error);
			fprintf(e ? stderr : stdout,
				"nv: partition=%u group=%u commit=%u ack_bytes=%zu error=%d\n",
				s.schedule.partition, frame.group, s.backend.storage_verified, sent,
				e);
			s.finishing = false;
			if (e)
				break;
		} else if (reply == NVC_PROGRESS || reply == NVC_NACK) {
			if ((e = nv_response_encode(&frame, reply == NVC_PROGRESS, ack)))
				break;
			if (write_exact(&s, ack, sizeof ack, &write_error) != sizeof ack ||
			    write_error) {
				e = write_error ? write_error : -EIO;
				break;
			}
			if (reply == NVC_NACK)
				s.transaction = false;
		} else if (!s.schedule.busy)
			s.transaction = false;
	}
	bool dirty = s.transaction || s.schedule.busy;
	nvc_discard(&s.schedule);
	nv_ubi_forget(&s.backend);
	nv_store_destroy(s.store);
	free(packet);
	pthread_mutex_lock(&w->mutex);
	w->status.discarded = dirty;
	pthread_mutex_unlock(&w->mutex);
	return (e == -ECANCELED || (e == NVU_OWNERSHIP && s.cancelled)) && worker_stopping(w) ? 0
											      : e;
}
