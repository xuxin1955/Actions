/* SPDX-License-Identifier: GPL-2.0-only */
#include "cpnv.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <libubox/blobmsg.h>
#include <sys/random.h>
#include <unistd.h>

static void describe(struct cpnv *c, struct blob_buf *b)
{
	const char *state = c->quiesced			   ? "quiesced"
			    : c->quiescing		   ? "quiescing"
			    : c->error			   ? "failed"
			    : c->nv_ready && c->halo_ready ? "running"
							   : "starting";
	blobmsg_add_string(b, "state", state);
	blobmsg_add_string(b, "storage", "ubi");
	blobmsg_add_u8(b, "nv_ready", c->nv_ready && !c->quiescing && !c->error);
	blobmsg_add_u8(b, "halo_ready", c->halo_ready && !c->error);
	blobmsg_add_u8(b, "ready", c->nv_ready && c->halo_ready && c->accepting && !c->error);
	blobmsg_add_u8(b, "accepting", c->accepting);
	blobmsg_add_u8(b, "nv_committing", c->committing);
	blobmsg_add_u8(b, "uncommitted_discarded", c->discarded);
	blobmsg_add_u32(b, "last_error", (uint32_t)(c->stop_error ? c->stop_error : c->error));
}

static int send_status(struct cpnv *c, struct ubus_request_data *request)
{
	struct blob_buf b = {0};
	blob_buf_init(&b, 0);
	describe(c, &b);
	int e = ubus_send_reply(&c->bus.ctx, request, b.head);
	blob_buf_free(&b);
	return e;
}

static void finished(struct cpnv *c, int error)
{
	c->stop_error = error;
	c->quiesced = !error;
	uloop_timeout_cancel(&c->stop_timeout);
	if (c->waiting) {
		send_status(c, &c->request);
		ubus_complete_deferred_request(&c->bus.ctx, &c->request,
					       error == -ETIMEDOUT ? UBUS_STATUS_TIMEOUT
					       : error		   ? UBUS_STATUS_UNKNOWN_ERROR
								   : UBUS_STATUS_OK);
		c->waiting = false;
	}
	if (c->terminate && c->joined) {
		c->result = error;
		uloop_end();
	}
}

static void ready(struct cpnv *c)
{
	if (c->error || c->quiescing || !c->nv_ready || !c->halo_ready || c->notify_started)
		return;
	int e = notify_start(c);
	if (e) {
		cpnv_fail(c, e);
		return;
	}
	c->notify_started = true;
	fprintf(stdout, "cpnv: ready\n");
	fflush(stdout);
}

void cpnv_halo_ready(struct cpnv *c)
{
	c->halo_ready = true;
	ready(c);
}

void cpnv_fail(struct cpnv *c, int error)
{
	if (c->error)
		return;
	c->error = error ? error : -EIO;
	c->accepting = false;
	modem_stop(c);
	notify_close(c);
	if (c->worker_started && !c->joined)
		worker_request_stop(&c->worker);
	fprintf(stderr, "cpnv: failed error=%d\n", c->error);
}

static void worker_event(struct uloop_fd *fd, unsigned events)
{
	struct cpnv *c = container_of(fd, struct cpnv, worker_event);
	uint64_t count;
	ssize_t n = read(fd->fd, &count, sizeof count);
	(void)n;
	(void)events;
	struct worker_status state;
	worker_snapshot(&c->worker, &state);
	c->nv_ready = state.nv_ready;
	c->committing = state.committing;
	c->discarded = state.discarded;
	if (state.error && !c->error)
		cpnv_fail(c, state.error);
	if (state.modem_started && !state.stopped && !c->modem && !c->quiescing && !c->error) {
		int e = modem_start(c);
		if (e)
			cpnv_fail(c, e);
	}
	if (state.stopped) {
		uloop_fd_delete(fd);
		worker_join(&c->worker);
		c->joined = true;
		if (c->quiescing)
			finished(c, state.error);
		else if (!c->error)
			cpnv_fail(c, -EIO);
	} else
		ready(c);
}

static void expired(struct uloop_timeout *timer)
{
	struct cpnv *c = container_of(timer, struct cpnv, stop_timeout);
	finished(c, -ETIMEDOUT);
}

static void begin_stop(struct cpnv *c)
{
	if (c->quiescing)
		return;
	c->quiescing = true;
	c->accepting = false;
	modem_stop(c);
	notify_close(c);
	if (!c->worker_started || c->joined) {
		c->quiesced = !c->error;
		finished(c, c->error);
		return;
	}
	worker_request_stop(&c->worker);
	uloop_timeout_set(&c->stop_timeout, 10000);
}

static int method(struct ubus_context *ctx, struct ubus_object *object,
		  struct ubus_request_data *request, const char *name, struct blob_attr *message)
{
	struct cpnv *c = container_of(object, struct cpnv, object);
	(void)message;
	if (!strcmp(name, "status"))
		return send_status(c, request);
	if (c->quiesced) {
		c->stop_error = 0;
		return send_status(c, request);
	}
	if (c->quiescing)
		return UBUS_STATUS_UNKNOWN_ERROR;
	ubus_defer_request(ctx, request, &c->request);
	c->waiting = true;
	begin_stop(c);
	return 0;
}

static const struct ubus_method methods[] = {UBUS_METHOD_NOARG("status", method),
					     UBUS_METHOD_NOARG("quiesce", method)};
static struct ubus_object_type type = UBUS_OBJECT_TYPE("udx.cpnv", methods);

static void connected(struct ubus_context *bus)
{
	struct cpnv *c = container_of(bus, struct cpnv, bus.ctx);
	int e = ubus_add_object(bus, &c->object);
	if (e) {
		fprintf(stderr, "cpnv: ubus registration error=%d\n", e);
		c->result = -EIO;
		uloop_end();
		return;
	}
	if (c->worker_started || c->quiescing)
		return;
	e = worker_start(&c->worker, &c->worker.policy);
	if (e) {
		cpnv_fail(c, e);
		return;
	}
	c->worker_started = true;
	c->worker_event.fd = c->worker.event;
	c->worker_event.cb = worker_event;
	uloop_fd_add(&c->worker_event, ULOOP_READ);
}

static void terminated(struct uloop_signal *signal)
{
	struct cpnv *c;
	if (signal->signo == SIGTERM)
		c = container_of(signal, struct cpnv, term);
	else
		c = container_of(signal, struct cpnv, interrupt);
	c->terminate = true;
	if (c->quiesced || c->joined || !c->worker_started) {
		c->result = c->error;
		uloop_end();
		return;
	}
	begin_stop(c);
}

int cpnv_service_run(struct cpnv *c, const struct cpnv_policy *policy)
{
	memset(c, 0, sizeof *c);
	c->worker.policy = *policy;
	c->worker_event.fd = -1;
	c->accepting = true;
	if (getrandom(&c->epoch, sizeof c->epoch, 0) != (ssize_t)sizeof c->epoch || !c->epoch)
		return -EIO;
	if (uloop_init())
		return -EIO;
	c->object = (struct ubus_object){.name = "udx.cpnv",
					 .type = &type,
					 .methods = methods,
					 .n_methods = sizeof methods / sizeof methods[0]};
	c->stop_timeout.cb = expired;
	c->term = (struct uloop_signal){.signo = SIGTERM, .cb = terminated};
	c->interrupt = (struct uloop_signal){.signo = SIGINT, .cb = terminated};
	uloop_signal_add(&c->term);
	uloop_signal_add(&c->interrupt);
	signal(SIGPIPE, SIG_IGN);
	c->bus.cb = connected;
	ubus_auto_connect(&c->bus);
	uloop_run();
	if (c->worker_started && !c->joined) {
		worker_request_stop(&c->worker);
		worker_join(&c->worker);
	}
	modem_close(c);
	notify_close(c);
	ubus_auto_shutdown(&c->bus);
	uloop_done();
	return c->result;
}
