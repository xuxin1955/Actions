/* SPDX-License-Identifier: GPL-2.0-only */
#include "cellular.h"
#include "network.h"
#include <errno.h>
#include <fcntl.h>
#include <libubox/blobmsg_json.h>
#include <libubus.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

struct control {
	struct transport *transport;
	pthread_mutex_t lock;
	pthread_t thread;
	struct ubus_auto_conn bus;
	struct ubus_request_data quiesce_request;
	struct uloop_timeout quiesce_timeout;
	bool quiesce_waiting, quiesce_requested;
	int pending_network_op;
	struct ubus_object object;
	struct ubus_object_type type;
	struct ubus_method *methods;
	struct uloop_fd wake;
	struct cell_network network;
	pthread_cond_t network_cond;
	J *snapshot;
	bool network_retired;
	atomic_bool done;
	int exit_status;
	struct controller *controller;
	atomic_bool accepting;
	bool busy, started;
	J *job, *arguments, *receipts[32];
	uint64_t sequence;
	char broker[33];
};

static atomic_bool running = true;
static atomic_bool terminating;
static _Atomic(struct transport *) active_transport;

static void stop(int sig)
{
	(void)sig;
	struct transport *transport = atomic_load(&active_transport);
	if (transport)
		atomic_store(&transport->stopping, true);
	running = 0;
	terminating = true;
	uloop_end();
}

/* The latest status file is not a receipt store: a second client can replace
 * it. */
static void remember(struct control *control, J *row)
{
	unsigned index = (unsigned)json_object_get_int64(get(row, "id")) % 32;
	json_object_put(control->receipts[index]);
	control->receipts[index] = jclone(row);
}

static J *dispatch(struct control *control, J *request)
{
	const char *const fields[] = {"operation", "arguments"};
	if (!keys(request, fields, 2)) {
		errorf(CELL_INVALID, "Invalid control request");
		return NULL;
	}
	const char *op = str(request, "operation");
	J *args = get(request, "arguments");
	if (controller_validate(op, args) < 0)
		return NULL;
	if (!strcmp(op, "operation_status")) {
		uint64_t id = (uint64_t)json_object_get_int64(get(args, "id"));
		pthread_mutex_lock(&control->lock);
		J *row = control->receipts[id % 32], *reply = NULL;
		if (row && json_object_equal(get(row, "id"), get(args, "id")) &&
		    text_equal(str(row, "boot_id"), str(args, "boot_id")) &&
		    text_equal(str(row, "broker_id"), str(args, "broker_id"))) {
			reply = jclone(row);
			boolean(reply, "accepted", true);
		} else
			errorf(CELL_UNCERTAIN, "Operation receipt expired or service restarted; "
					       "request was not replayed");
		pthread_mutex_unlock(&control->lock);
		return reply;
	}
	if (!strcmp(op, "status")) {
		pthread_mutex_lock(&control->lock);
		J *value = jclone(control->snapshot);
		pthread_mutex_unlock(&control->lock);
		if (!value)
			return NULL;
		boolean(value, "accepted", true);
		boolean(value, "cached", true);
		boolean(value, "service_stopped", atomic_load(&control->done));
		boolean(value, "available", true);
		double age = mono() - json_object_get_double(get(value, "uptime"));
		real(value, "snapshot_age_seconds", age < 0 ? 0 : age);
		boolean(value, "stale", age > 35);
		boolean(value, "available", age >= 0 && age <= 35);
		return value;
	}
	pthread_mutex_lock(&control->lock);
	J *reply = NULL, *row = NULL;
	if (!atomic_load(&control->accepting)) {
		errorf(CELL_SYSTEM, "Cellular service is stopping");
		goto end;
	}
	if (control->transport->poisoned) {
		errorf(CELL_UNCERTAIN, "AT outcome uncertain; no automatic replay");
		goto end;
	}
	if (control->busy || control->job) {
		reply = json_object_new_object();
		boolean(reply, "accepted", false);
		boolean(reply, "busy", true);
		string(reply, "error", "Another cellular operation is running");
		goto end;
	}
	row = json_object_new_object();
	integer(row, "id", ++control->sequence);
	string(row, "boot_id", control->transport->boot);
	string(row, "broker_id", control->broker);
	string(row, "operation", op);
	string(row, "state", "queued");
	real(row, "queued_uptime", mono());
	if (!strcmp(op, "sms_send")) {
		int state;
		J *old = receipt_reserve(op, args, row, &state);
		if (state < 0)
			goto end;
		if (old) {
			reply = json_object_new_object();
			boolean(reply, "accepted", true);
			boolean(reply, "duplicate", true);
			json_object_object_add(reply, "operation_id",
					       json_object_get(get(old, "id")));
			json_object_object_add(reply, "original_boot_id",
					       json_object_get(get(old, "boot_id")));
			json_object_object_add(reply, "state", json_object_get(get(old, "state")));
			json_object_object_add(reply, "result",
					       json_object_get(get(old, "result")));
			json_object_object_add(reply, "error", json_object_get(get(old, "error")));
			if (!text_equal(str(old, "broker_id"), control->broker) &&
			    (text_equal(str(old, "state"), "queued") ||
			     text_equal(str(old, "state"), "running"))) {
				string(reply, "state", "uncertain");
				string(reply, "error",
				       "Previous broker ended without a terminal receipt; request "
				       "will not be "
				       "replayed");
			}
			json_object_put(old);
			goto end;
		}
	}

	remember(control, row);
	control->job = json_object_get(row);
	control->arguments = jclone(args);
	reply = json_object_new_object();
	boolean(reply, "accepted", true);
	integer(reply, "operation_id", control->sequence);
	string(reply, "broker_id", control->broker);
	string(reply, "boot_id", control->transport->boot);
	string(reply, "state", "queued");
end:
	json_object_put(row);
	pthread_mutex_unlock(&control->lock);
	return reply;
}

/* Ubus registration and event dispatch are below the worker helpers. */
static J *take(struct control *control, J **args)
{
	pthread_mutex_lock(&control->lock);
	J *job = control->job;
	*args = control->arguments;
	control->job = control->arguments = NULL;
	if (job)
		control->busy = true;
	pthread_mutex_unlock(&control->lock);
	return job;
}

static bool foreground_pending(void *opaque)
{
	struct control *control = opaque;
	pthread_mutex_lock(&control->lock);
	bool pending = control->job != NULL || control->busy;
	pthread_mutex_unlock(&control->lock);
	return pending || json_object_array_length(control->transport->sms_events) > 0;
}

static int finish(struct control *control, J *row)
{
	pthread_mutex_lock(&control->lock);
	int rc = receipt_finish(row);

	remember(control, row);
	control->busy = false;
	pthread_mutex_unlock(&control->lock);
	return rc;
}

static void failed(struct controller *c, const char *prefix)
{
	char saved[512];
	snprintf(saved, sizeof(saved), "%s", cell_error);
	snprintf(c->error, sizeof(c->error), "%s%.450s", prefix ? prefix : "", saved);
	if (c->transport.poisoned)
		controller_set_data(c, NULL);
	else
		controller_publish(c);
}

static void *worker(void *opaque)
{
	struct control *shared = opaque;
	struct controller *c = shared->controller;
	double last = 0, next_connect = 0, next_reopen = 0;
	int exit_status = 0;
	while (running) {
		double now = mono();
		J *args = NULL, *job = take(shared, &args);
		if (c->transport.poisoned && now >= next_reopen && running) {
			if (transport_reopen(&c->transport) < 0)
				snprintf(c->error, sizeof(c->error), "%s", cell_error);
			else {
				c->initialized = c->ims_initialized = false;
				memset(c->telemetry_due, 0, sizeof(c->telemetry_due));
				c->error[0] = 0;
				controller_publish(c);
			}
			next_reopen = mono() + 15;
		}
		if (job) {
			string(job, "state", "running");
			real(job, "started_uptime", mono());
			pthread_mutex_lock(&shared->lock);
			remember(shared, job);
			pthread_mutex_unlock(&shared->lock);
			J *result = NULL;
			if (running)
				result = controller_execute(c, str(job, "operation"), args);
			else
				errorf(CELL_SYSTEM, "Cancelled before execution by service stop");
			if (result) {
				json_object_object_add(job, "result", result);
				bool debug_failed =
				    text_equal(str(job, "operation"), "at_debug") &&
				    !json_object_get_boolean(get(result, "success"));
				string(job, "state",
				       !debug_failed	       ? "complete"
				       : c->transport.poisoned ? "uncertain"
							       : "failed");
				if (debug_failed)
					string(job, "error", str(result, "error"));
			} else {
				string(job, "state",
				       c->transport.poisoned ? "uncertain" : "failed");
				string(job, "error", cell_error);
				failed(c, NULL);
			}
			real(job, "finished_uptime", mono());
			if (finish(shared, job) < 0) {
				failed(c, "Receipt persistence: ");
				exit_status = 1;
				running = 0;
			}
			controller_publish(c);
			last = 0;
			json_object_put(job);
			json_object_put(args);
			if (!running)
				break;
		}
		if (!c->transport.poisoned && c->radio_restore_pending && mono() - last >= 10) {
			if (radio_restore(c, c->restore_radio, c->restore_data) < 0)
				failed(c, "Radio restoration: ");
		}
		if (!c->transport.poisoned && !radio_enabled(c) && mono() - last >= 10 &&
		    radio_restore(c, false, c->want_data) < 0)
			failed(c, "Radio-off policy: ");
		if (!c->transport.poisoned && mono() - last >= 10) {
			c->background = true;
			c->background_deferred = false;
			if (controller_refresh(c) < 0 && !c->background_deferred)
				failed(c, NULL);
			c->background = false;
			last = c->background_deferred ? 0 : mono();
		}
		if (radio_enabled(c) && !c->radio_restore_pending && c->want_data && !c->data &&
		    !c->transport.poisoned && mono() >= next_connect) {
			J *result = controller_connect(c, 10);
			if (!result) {
				failed(c, NULL);
				controller_set_data(c, NULL);
			}
			json_object_put(result);
			next_connect = mono() + 15;
		}
		if (!c->transport.poisoned &&
		    (transport_drain(&c->transport) < 0 || sms_process_events(c) < 0))
			failed(c, "SMS receive: ");
		if (!c->transport.poisoned && telemetry_step(c) < 0)
			failed(c, "Telemetry: ");
		if (c->transport.session_dead) {
			exit_status = 1;
			break;
		}
		delay_ms(200);
	}
	atomic_store(&shared->accepting, false);
	c->want_data = false;
	char shutdown_error[512] = "";
	{
		J *result = controller_disconnect(c, true);
		if (!result) {
			snprintf(shutdown_error, sizeof(shutdown_error), "%s", cell_error);
			failed(c, "shutdown: ");
			controller_set_data(c, NULL);
		}
		json_object_put(result);
	}
	transport_close(&c->transport);
	if (*shutdown_error)
		exit_status = 1;
	shared->exit_status = exit_status;
	atomic_store(&shared->done, true);
	uint64_t event = 1;
	(void)write(shared->wake.fd, &event, sizeof(event));
	return NULL;
}

static void network_retired(void *opaque, bool retired)
{
	struct control *control = opaque;
	pthread_mutex_lock(&control->lock);
	control->network_retired = retired;
	pthread_cond_broadcast(&control->network_cond);
	pthread_mutex_unlock(&control->lock);
}

static int retire_network(void *opaque)
{
	struct control *control = opaque;
	struct timespec end;
	clock_gettime(CLOCK_MONOTONIC, &end);
	end.tv_sec += 8;
	pthread_mutex_lock(&control->lock);
	int ret = 0;
	while (!control->network_retired && !ret)
		ret = pthread_cond_timedwait(&control->network_cond, &control->lock, &end);
	pthread_mutex_unlock(&control->lock);
	return ret ? errorf(CELL_SYSTEM, "netifd did not retire the data session") : 0;
}

static int publish_state(void *opaque, J *state)
{
	struct control *control = opaque;
	J *copy = jclone(state);
	if (!copy)
		return errorf(CELL_SYSTEM, "Status allocation failed");
	pthread_mutex_lock(&control->lock);
	bool had_data = get(control->snapshot, "data_session") != NULL;
	json_object_put(control->snapshot);
	control->snapshot = copy;
	if (had_data && !get(copy, "data_session"))
		control->network_retired = false;
	pthread_mutex_unlock(&control->lock);
	uint64_t event = 1;
	if (write(control->wake.fd, &event, sizeof(event)) < 0 && errno != EAGAIN)
		return errorf(CELL_SYSTEM, "Status wakeup failed");
	return 0;
}

static void wakeup(struct uloop_fd *fd, unsigned events)
{
	struct control *control = container_of(fd, struct control, wake);
	(void)events;
	uint64_t count;
	while (read(fd->fd, &count, sizeof(count)) > 0) {
	}
	pthread_mutex_lock(&control->lock);
	J *snapshot = jclone(control->snapshot);
	pthread_mutex_unlock(&control->lock);
	cell_network_update(&control->network, get(snapshot, "data_session"));
	json_object_put(snapshot);
	if (control->pending_network_op && atomic_load(&control->accepting)) {
		J *args = json_object_new_object(), *request = json_object_new_object();
		bool connect = control->pending_network_op == 1;
		if (connect)
			boolean(args, "automatic", true);
		string(request, "operation", connect ? "connect" : "disconnect");
		json_object_object_add(request, "arguments", args);
		J *reply = dispatch(control, request);
		if (reply && !json_object_get_boolean(get(reply, "busy")))
			control->pending_network_op = 0;
		json_object_put(reply);
		json_object_put(request);
	}
	if (atomic_load(&control->done)) {
		uloop_timeout_cancel(&control->quiesce_timeout);
		if (control->quiesce_waiting) {
			ubus_complete_deferred_request(
			    &control->bus.ctx, &control->quiesce_request,
			    control->exit_status ? UBUS_STATUS_UNKNOWN_ERROR : UBUS_STATUS_OK);
			control->quiesce_waiting = false;
		}
		/* Keep the ubus connection until procd terminates the service.
		 * Closing it with the reply queued can make ubusd observe EOF first. */
		if (terminating || !control->quiesce_requested)
			uloop_end();
	}
}

static J *stored_status(struct control *control, const char *file)
{
	char path[256];
	snprintf(path, sizeof(path), CELL_RUN "/%s", file);
	char *text = read_text(path, 524288, false);
	J *value = text ? parse_json_document(text, strlen(text)) : NULL;
	free(text);
	if (value && (!text_equal(str(value, "boot_id"), control->transport->boot) ||
		      (strcmp(file, "sms-history.json") &&
		       (mono() - json_object_get_double(get(value, "uptime")) > 300 ||
			mono() < json_object_get_double(get(value, "uptime")))))) {
		json_object_put(value);
		value = NULL;
	}
	if (!value) {
		value = json_object_new_object();
		boolean(value, "available", false);
		string(value, "error", "Not sampled yet");
	} else
		boolean(value, "available", true);
	return value;
}

static J *send_receipt(struct control *control, J *args)
{
	const char *id = str(args, "request_id");
	if (!hex_string(id, 32))
		return NULL;
	J *row = NULL;
	pthread_mutex_lock(&control->lock);
	for (unsigned i = 0; i < 32; i++)
		if (text_equal(str(control->receipts[i], "request_id"), id)) {
			row = jclone(control->receipts[i]);
			break;
		}
	pthread_mutex_unlock(&control->lock);
	if (!row) {
		char path[512];
		snprintf(path, sizeof(path), CELL_STORE "/requests/%s.json", id);
		row = read_json(path, 262144);
		if (text_equal(str(row, "state"), "queued") ||
		    text_equal(str(row, "state"), "running")) {
			string(row, "state", "uncertain");
			string(row, "error",
			       "Prior operation ended without a final receipt; not replayed");
		}
	}
	if (!row) {
		row = json_object_new_object();
		boolean(row, "available", false);
	} else
		boolean(row, "available", true);
	string(row, "request_id", id);
	J *progress = read_json(CELL_RUN "/sms-send-progress.json", 262144);
	if (text_equal(str(progress, "request_id"), id))
		json_object_object_add(row, "submission_progress", json_object_get(progress));
	json_object_put(progress);
	return row;
}

static void quiesce_expired(struct uloop_timeout *timer)
{
	struct control *c = container_of(timer, struct control, quiesce_timeout);
	if (c->quiesce_waiting) {
		ubus_complete_deferred_request(&c->bus.ctx, &c->quiesce_request,
					       UBUS_STATUS_TIMEOUT);
		c->quiesce_waiting = false;
	}
}

static int quiesce(struct control *c, struct ubus_request_data *req)
{
	if (atomic_load(&c->done))
		return c->exit_status ? UBUS_STATUS_UNKNOWN_ERROR : UBUS_STATUS_OK;
	if (!atomic_exchange(&c->accepting, false))
		return UBUS_STATUS_UNKNOWN_ERROR;
	c->quiesce_requested = true;
	atomic_store(&c->transport->stopping, true);
	running = false;
	pthread_mutex_lock(&c->lock);
	J *job = c->job;
	c->job = NULL;
	json_object_put(c->arguments);
	c->arguments = NULL;
	if (job) {
		string(job, "state", "failed");
		string(job, "error", "Cancelled before execution by service stop");
		real(job, "finished_uptime", mono());
		(void)receipt_finish(job);
		remember(c, job);
		json_object_put(job);
	}
	pthread_mutex_unlock(&c->lock);
	ubus_defer_request(&c->bus.ctx, req, &c->quiesce_request);
	c->quiesce_waiting = true;
	c->quiesce_timeout.cb = quiesce_expired;
	uloop_timeout_set(&c->quiesce_timeout, 10000);
	return 0;
}

static int bus_method(struct ubus_context *ctx, struct ubus_object *object,
		      struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	struct control *control = container_of(object, struct control, object);
	if (!strcmp(method, "quiesce"))
		return quiesce(control, req);
	char *text = msg ? blobmsg_format_json(msg, true) : strdup("{}");
	J *args = text ? parse_json(text, strlen(text)) : NULL;
	free(text);
	if (!args || !json_object_is_type(args, json_type_object)) {
		json_object_put(args);
		return UBUS_STATUS_INVALID_ARGUMENT;
	}
	json_object_object_del(args, "ubus_rpc_session");
	cell_error[0] = 0;
	J *reply = NULL;
	if (!strcmp(method, "sms_status"))
		reply = stored_status(control, "sms-status.json");
	else if (!strcmp(method, "sms_history_status"))
		reply = stored_status(control, "sms-history.json");
	else if (!strcmp(method, "apn_status"))
		reply = stored_status(control, "apn-status.json");
	else if (!strcmp(method, "sms_receipt"))
		reply = send_receipt(control, args);
	else if (!strcmp(method, "network_setup") || !strcmp(method, "network_teardown")) {
		int ret;
		if (!strcmp(method, "network_setup")) {
			if (!json_object_is_type(get(args, "metric"), json_type_int) ||
			    json_object_get_int64(get(args, "metric")) < 0 ||
			    json_object_get_int64(get(args, "metric")) > 65535) {
				json_object_put(args);
				return UBUS_STATUS_INVALID_ARGUMENT;
			}
			ret = cell_network_setup(&control->network, str(args, "interface"),
						 str(args, "device"),
						 json_object_get_int(get(args, "metric")));
		} else
			ret = cell_network_teardown(&control->network, str(args, "interface"));
		if (!ret) {
			control->pending_network_op = !strcmp(method, "network_setup") ? 1 : 2;
			uint64_t event = 1;
			(void)write(control->wake.fd, &event, sizeof(event));
		}
		reply = json_object_new_object();
		boolean(reply, "accepted", !ret);
		integer(reply, "generation", control->network.generation);
	} else {
		J *request = json_object_new_object();
		string(request, "operation", method);
		json_object_object_add(request, "arguments", json_object_get(args));
		reply = dispatch(control, request);
		json_object_put(request);
	}
	json_object_put(args);
	if (!reply) {
		reply = json_object_new_object();
		boolean(reply, "accepted", false);
		string(reply, "error", *cell_error ? cell_error : "Invalid request");
	}
	integer(reply, "pid", getpid());
	struct blob_buf b = {0};
	blob_buf_init(&b, 0);
	blobmsg_add_json_from_string(&b, json_object_to_json_string_ext(reply, 0));
	int ret = ubus_send_reply(ctx, req, b.head);
	blob_buf_free(&b);
	json_object_put(reply);
	return ret;
}

static void connected(struct ubus_context *ctx);

static void reconnect_bus(struct uloop_timeout *timer)
{
	struct ubus_auto_conn *bus = container_of(timer, struct ubus_auto_conn, timer);
	if (ubus_reconnect(&bus->ctx, bus->path)) {
		uloop_timeout_set(timer, 1000);
		return;
	}
	connected(&bus->ctx);
	ubus_add_uloop(&bus->ctx);
}

static void disconnected(struct ubus_context *ctx)
{
	struct control *c =
	    container_of(container_of(ctx, struct ubus_auto_conn, ctx), struct control, bus);
	c->quiesce_waiting = false;
	cell_network_detach(&c->network);
	c->bus.timer.cb = reconnect_bus;
	uloop_timeout_set(&c->bus.timer, 1000);
}

static void connected(struct ubus_context *ctx)
{
	struct ubus_auto_conn *auto_conn = container_of(ctx, struct ubus_auto_conn, ctx);
	struct control *control = container_of(auto_conn, struct control, bus);
	if (control->network.bus)
		cell_network_detach(&control->network);
	if (!control->object.id && ubus_add_object(ctx, &control->object)) {
		syslog(LOG_ERR, "Cannot register udx.cellular");
		running = 0;
		return;
	}
	ctx->connection_lost = disconnected;
	cell_network_attach(&control->network, ctx);
}

static int control_start(struct control *control, struct transport *transport)
{
	memset(control, 0, sizeof(*control));
	control->wake.fd = -1;
	control->transport = transport;
	pthread_mutex_init(&control->lock, NULL);
	pthread_condattr_t attr;
	pthread_condattr_init(&attr);
	pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
	pthread_cond_init(&control->network_cond, &attr);
	pthread_condattr_destroy(&attr);
	atomic_init(&control->accepting, true);
	atomic_init(&control->done, false);
	control->network_retired = true;
	if (random_hex(control->broker, 16) < 0)
		return -1;
	control->wake.fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (control->wake.fd < 0)
		return -1;
	control->wake.cb = wakeup;
	if (uloop_fd_add(&control->wake, ULOOP_READ))
		return -1;
	J *names = controller_operations();
	const char *extra[] = {"quiesce",     "sms_status",    "sms_history_status", "apn_status",
			       "sms_receipt", "network_setup", "network_teardown"};
	size_t count = json_object_array_length(names),
	       total = count + sizeof(extra) / sizeof(*extra);
	control->methods = calloc(total, sizeof(*control->methods));
	if (!control->methods) {
		json_object_put(names);
		return -1;
	}
	for (size_t i = 0; i < total; i++) {
		control->methods[i].name =
		    strdup(i < count ? json_object_get_string(json_object_array_get_idx(names, i))
				     : extra[i - count]);
		control->methods[i].handler = bus_method;
	}
	json_object_put(names);
	control->type = (struct ubus_object_type){
	    .name = "udx.cellular", .methods = control->methods, .n_methods = total};
	control->object = (struct ubus_object){.name = "udx.cellular",
					       .type = &control->type,
					       .methods = control->methods,
					       .n_methods = total};
	control->network.retired = network_retired;
	control->network.opaque = control;
	control->bus.path = getenv("UBUS_SOCKET");
	control->bus.cb = connected;
	ubus_auto_connect(&control->bus);
	return 0;
}

static void control_close(struct control *control)
{
	uloop_timeout_cancel(&control->quiesce_timeout);
	cell_network_close(&control->network);
	ubus_auto_shutdown(&control->bus);
	if (control->wake.fd >= 0) {
		uloop_fd_delete(&control->wake);
		close(control->wake.fd);
	}
	json_object_put(control->job);
	json_object_put(control->arguments);
	json_object_put(control->snapshot);
	for (unsigned i = 0; i < 32; i++)
		json_object_put(control->receipts[i]);
	for (int i = 0; i < control->object.n_methods; i++)
		free((void *)control->methods[i].name);
	free(control->methods);
	pthread_cond_destroy(&control->network_cond);
	pthread_mutex_destroy(&control->lock);
}

static void cpnv_status(struct ubus_request *request, int type, struct blob_attr *msg)
{
	(void)type;
	struct blob_attr *a;
	size_t rem;
	if (!msg)
		return;
	blobmsg_for_each_attr(a, msg, rem) if (!strcmp(blobmsg_name(a), "ready") &&
					       blobmsg_type(a) == BLOBMSG_TYPE_BOOL) *
	    (bool *)request->priv = blobmsg_get_bool(a);
}

static bool cpnv_ready(void)
{
	struct ubus_context *ctx = ubus_connect(getenv("UBUS_SOCKET"));
	if (!ctx)
		return false;
	uint32_t id;
	bool ready = false;
	if (!ubus_lookup_id(ctx, "udx.cpnv", &id))
		ubus_invoke(ctx, id, "status", NULL, cpnv_status, &ready, 2000);
	ubus_free(ctx);
	return ready;
}

int main(void)
{
	uloop_init();
	signal(SIGTERM, stop);
	signal(SIGINT, stop);
	signal(SIGPIPE, SIG_IGN);
	openlog("udx-cellular", LOG_PID, LOG_DAEMON);
	double deadline = mono() + 180;
	while (!cpnv_ready()) {
		if (!running || mono() >= deadline) {
			fprintf(stderr, "CPNV was not ready before startup deadline\n");
			return 1;
		}
		delay_ms(500);
	}
	struct controller *c = calloc(1, sizeof(*c));
	if (!c || controller_init(c) < 0) {
		fprintf(stderr, "%s\n", cell_error);
		if (c)
			controller_destroy(c);
		free(c);
		return 1;
	}
	active_transport = &c->transport;
	struct control control;
	if (control_start(&control, &c->transport)) {
		active_transport = NULL;
		controller_destroy(c);
		free(c);
		return 1;
	}
	control.controller = c;
	c->foreground_pending = foreground_pending;
	c->foreground_context = &control;
	c->publish_state = publish_state;
	c->retire_network = retire_network;
	c->service_context = &control;
	if (controller_publish(c) < 0) {
		control_close(&control);
		active_transport = NULL;
		controller_destroy(c);
		free(c);
		return 1;
	}
	J *history = sms_publish(c);
	if (!history) {
		fprintf(stderr, "%s\n", cell_error);
		control_close(&control);
		active_transport = NULL;
		controller_destroy(c);
		free(c);
		return 1;
	}
	json_object_put(history);
	int rc = pthread_create(&control.thread, NULL, worker, &control);
	if (rc) {
		control_close(&control);
		active_transport = NULL;
		controller_destroy(c);
		free(c);
		return 1;
	}
	while (!atomic_load(&control.done) || (control.quiesce_requested && !terminating)) {
		uloop_run();
		if (terminating)
			running = 0;
	}
	pthread_join(control.thread, NULL);
	rc = control.exit_status;
	control_close(&control);
	active_transport = NULL;
	controller_destroy(c);
	free(c);
	uloop_done();
	closelog();
	return rc;
}
