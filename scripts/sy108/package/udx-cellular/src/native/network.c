/* SPDX-License-Identifier: GPL-2.0-only */
#include "network.h"
#include <libubox/blobmsg_json.h>
#include <stdlib.h>
#include <string.h>

static void drive(struct cell_network *n);

static void add_address(J *array, const char *ip, const char *mask)
{
	if (!ip)
		return;
	J *row = json_object_new_object();
	string(row, "ipaddr", ip);
	string(row, "mask", mask);
	json_object_array_add(array, row);
}

static J *notification(struct cell_network *n, J *session)
{
	J *o = json_object_new_object();
	integer(o, "action", 0);
	string(o, "interface", "wan");
	string(o, "ifname", "wwan0");
	boolean(o, "link-up", session != NULL);
	boolean(o, "keep", false);
	if (session) {
		J *v4 = json_object_new_array(), *v6 = json_object_new_array();
		J *routes = json_object_new_array(), *data = json_object_new_object();
		add_address(v4, str(session, "ipv4"), "32");
		add_address(v6, str(session, "ipv6_linklocal"), "64");
		add_address(v6, str(session, "ipv6"), "64");
		if (str(session, "ipv4")) {
			J *route = json_object_new_object();
			string(route, "target", "0.0.0.0");
			string(route, "netmask", "0");
			integer(route, "metric", n->metric);
			json_object_array_add(routes, route);
		}
		json_object_object_add(o, "ipaddr", v4);
		json_object_object_add(o, "ip6addr", v6);
		json_object_object_add(o, "routes", routes);
		json_object_object_add(o, "dns", json_object_get(get(session, "dns")));
		integer(data, "cid", 1);
		string(data, "apn", str(session, "apn"));
		string(data, "session_id", str(session, "id"));
		json_object_object_add(o, "data", data);
	}
	return o;
}

static void status_reply(struct ubus_request *request, int type, struct blob_attr *msg)
{
	struct cell_network *n = request->priv;
	(void)type;
	char *text = msg ? blobmsg_format_json(msg, true) : NULL;
	J *state = text ? parse_json(text, strlen(text)) : NULL;
	free(text);
	n->down_confirmed = state && get(state, "up") && !json_object_get_boolean(get(state, "up"));
	const char *fields[] = {"ipv4-address", "ipv6-address", "route"};
	for (unsigned i = 0; i < 3; i++) {
		J *list = get(state, fields[i]);
		if (list &&
		    (!json_object_is_type(list, json_type_array) || json_object_array_length(list)))
			n->down_confirmed = false;
	}
	json_object_put(state);
}

static void complete(struct ubus_request *request, int status);

static int invoke(struct cell_network *n, const char *object, const char *method, J *args)
{
	uint32_t id;
	if (!n->bus || ubus_lookup_id(n->bus, object, &id))
		return -1;
	struct blob_buf b = {0};
	blob_buf_init(&b, 0);
	if (args)
		blobmsg_add_json_from_string(&b, json_object_to_json_string_ext(args, 0));
	memset(&n->request, 0, sizeof(n->request));
	int ret = ubus_invoke_async(n->bus, id, method, b.head, &n->request);
	blob_buf_free(&b);
	if (ret)
		return ret;
	n->request.priv = n;
	n->request.complete_cb = complete;
	n->request.data_cb = n->querying ? status_reply : NULL;
	n->request_generation = n->generation;
	n->pending = true;
	ubus_complete_request_async(n->bus, &n->request);
	return 0;
}

static void retry(struct uloop_timeout *timer)
{
	struct cell_network *n = container_of(timer, struct cell_network, retry);
	if (n->pending) {
		ubus_abort_request(n->bus, &n->request);
		n->pending = false;
		n->querying = false;
		n->retired(n->opaque, false);
	}
	drive(n);
}

static void complete(struct ubus_request *request, int status)
{
	struct cell_network *n = request->priv;
	n->pending = false;
	uloop_timeout_cancel(&n->retry);
	if (n->request_generation != n->generation)
		return;
	if (!n->sent && !n->querying) {
		n->querying = true;
		n->down_confirmed = false;
		if (!invoke(n, "network.interface.wan", "status", NULL)) {
			uloop_timeout_set(&n->retry, 2000);
			return;
		}
		status = UBUS_STATUS_NOT_FOUND;
	}
	if (status || (n->querying && !n->down_confirmed)) {
		n->querying = false;
		n->retired(n->opaque, false);
		uloop_timeout_set(&n->retry, 1000);
		return;
	}
	n->querying = false;
	json_object_put(n->applied);
	n->applied = jclone(n->sent);
	n->retired(n->opaque, !n->applied);
	drive(n);
}

static void drive(struct cell_network *n)
{
	if (!n->known) {
		n->retired(n->opaque, true);
		return;
	}
	if (!n->bus || n->pending)
		return;
	J *wanted = n->active ? n->desired : NULL;
	if (json_object_equal(wanted, n->applied) && (wanted || n->down_confirmed))
		return;
	/* Retire the old addresses before installing a different session. */
	json_object_put(n->sent);
	n->sent = n->applied ? NULL : jclone(wanted);
	J *args = notification(n, n->sent);
	n->querying = false;
	if (invoke(n, "network.interface", "notify_proto", args)) {
		n->retired(n->opaque, false);
		uloop_timeout_set(&n->retry, 1000);
	} else {
		uloop_timeout_set(&n->retry, 2000);
	}
	json_object_put(args);
}

static void renew_complete(struct ubus_request *request, int status)
{
	struct cell_network *n = request->priv;
	(void)status;
	n->renew_pending = false;
}

static void recover(struct cell_network *n)
{
	uint32_t id;
	if (n->renew_pending || ubus_lookup_id(n->bus, "network.interface.wan", &id))
		return;
	struct blob_buf b = {0};
	blob_buf_init(&b, 0);
	memset(&n->renew, 0, sizeof(n->renew));
	if (!ubus_invoke_async(n->bus, id, "renew", b.head, &n->renew)) {
		n->renew.priv = n;
		n->renew.complete_cb = renew_complete;
		n->renew_pending = true;
		ubus_complete_request_async(n->bus, &n->renew);
	}
	blob_buf_free(&b);
}

static void object_added(struct ubus_context *ctx, struct ubus_event_handler *ev, const char *type,
			 struct blob_attr *msg)
{
	struct cell_network *n = container_of(ev, struct cell_network, events);
	(void)ctx;
	(void)type;
	char *text = blobmsg_format_json(msg, true);
	J *o = text ? parse_json(text, strlen(text)) : NULL;
	free(text);
	if (n->bus && text_equal(str(o, "path"), "network.interface.wan"))
		recover(n);
	json_object_put(o);
}

void cell_network_attach(struct cell_network *n, struct ubus_context *ctx)
{
	n->bus = ctx;
	n->retry.cb = retry;
	n->events.cb = object_added;
	ubus_register_event_handler(ctx, &n->events, "ubus.object.add");
	recover(n);
	drive(n);
}

void cell_network_detach(struct cell_network *n)
{
	uloop_timeout_cancel(&n->retry);
	if (n->pending)
		ubus_abort_request(n->bus, &n->request);
	if (n->renew_pending)
		ubus_abort_request(n->bus, &n->renew);
	n->pending = n->renew_pending = false;
	n->bus = NULL;
	n->down_confirmed = false;
	n->retired(n->opaque, false);
}

int cell_network_setup(struct cell_network *n, const char *interface, const char *device,
		       int metric)
{
	if (!text_equal(interface, "wan") || !text_equal(device, "wwan0") || metric < 0 ||
	    metric > 65535)
		return -1;
	if (n->pending)
		ubus_abort_request(n->bus, &n->request);
	n->pending = false;
	n->generation++;
	n->metric = metric;
	n->known = true;
	n->active = true;
	/* A new protocol binding must not inherit an acknowledgement from the
	 * previous netifd instance. Retire before replaying the full snapshot. */
	json_object_put(n->applied);
	n->applied = json_object_new_object();
	n->down_confirmed = false;
	n->retired(n->opaque, false);
	drive(n);
	return 0;
}

int cell_network_teardown(struct cell_network *n, const char *interface)
{
	if (!text_equal(interface, "wan"))
		return -1;
	if (n->pending)
		ubus_abort_request(n->bus, &n->request);
	n->pending = false;
	n->generation++;
	n->active = false;
	n->down_confirmed = false;
	drive(n);
	return 0;
}

void cell_network_update(struct cell_network *n, J *session)
{
	json_object_put(n->desired);
	n->desired = jclone(session);
	drive(n);
}

void cell_network_close(struct cell_network *n)
{
	cell_network_detach(n);
	json_object_put(n->desired);
	json_object_put(n->sent);
	json_object_put(n->applied);
}
