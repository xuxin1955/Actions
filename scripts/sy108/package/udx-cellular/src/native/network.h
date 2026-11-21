/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef CELL_NETWORK_H
#define CELL_NETWORK_H
#include "common.h"
#include <libubox/uloop.h>
#include <libubus.h>

struct cell_network {
	struct ubus_context *bus;
	struct ubus_request request, renew;
	struct ubus_event_handler events;
	struct uloop_timeout retry;
	J *desired, *sent, *applied;
	unsigned generation, request_generation;
	int metric;
	bool known, active, pending, querying, down_confirmed, renew_pending;
	void (*retired)(void *, bool);
	void *opaque;
};

void cell_network_attach(struct cell_network *, struct ubus_context *);
void cell_network_detach(struct cell_network *);
int cell_network_setup(struct cell_network *, const char *, const char *, int);
int cell_network_teardown(struct cell_network *, const char *);
void cell_network_update(struct cell_network *, J *);
void cell_network_close(struct cell_network *);
#endif
