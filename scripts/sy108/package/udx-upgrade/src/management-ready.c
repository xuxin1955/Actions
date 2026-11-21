/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "management-ready.h"
#include <arpa/inet.h>
#include <errno.h>
#include <libubus.h>
#include <libubox/blobmsg.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <uci.h>
#include <unistd.h>

struct management_ready {
	uint32_t address;
	unsigned prefix;
	int network, logd, dnsmasq, pool, metrics;
	char bus_name[128];
};

static struct blob_attr *field(struct blob_attr *table, const char *name, int type)
{
	struct blob_attr *a;
	size_t rem;
	if (!table)
		return NULL;
	blobmsg_for_each_attr(a, table, rem) if (blobmsg_check_attr(a, true) &&
						 blobmsg_type(a) == type &&
						 !strcmp(blobmsg_name(a), name)) return a;
	return NULL;
}

static int ipv4(const char *text, uint32_t *value)
{
	struct in_addr address;
	if (inet_pton(AF_INET, text, &address) != 1)
		return -1;
	*value = ntohl(address.s_addr);
	return 0;
}

static int configured_address(uint32_t *address)
{
	struct uci_context *ctx = uci_alloc_context();
	struct uci_ptr ptr = {};
	char key[] = "network.usbmgmt.ipaddr";
	int ret = -1;
	if (!ctx)
		return -1;
	if (!uci_lookup_ptr(ctx, &ptr, key, true) && ptr.o && ptr.o->type == UCI_TYPE_STRING)
		ret = ipv4(ptr.o->v.string, address);
	uci_free_context(ctx);
	return ret;
}

static void network_reply(struct ubus_request *request, int type, struct blob_attr *message)
{
	struct management_ready *s = request->priv;
	(void)type;
	struct blob_attr *up = field(message, "up", BLOBMSG_TYPE_BOOL);
	struct blob_attr *dev = field(message, "l3_device", BLOBMSG_TYPE_STRING);
	struct blob_attr *proto = field(message, "proto", BLOBMSG_TYPE_STRING);
	struct blob_attr *addresses = field(message, "ipv4-address", BLOBMSG_TYPE_ARRAY), *item;
	size_t rem;
	if (!up || !blobmsg_get_bool(up) || !dev || strcmp(blobmsg_get_string(dev), "br-usb") ||
	    !proto || strcmp(blobmsg_get_string(proto), "static") || !addresses)
		return;
	blobmsg_for_each_attr(item, addresses, rem)
	{
		struct blob_attr *a = field(item, "address", BLOBMSG_TYPE_STRING);
		struct blob_attr *m = field(item, "mask", BLOBMSG_TYPE_INT32);
		uint32_t address;
		if (a && m && !ipv4(blobmsg_get_string(a), &address) && address == s->address) {
			s->prefix = blobmsg_get_u32(m);
			s->network = s->prefix >= 1 && s->prefix <= 30;
			return;
		}
	}
}

/* Read the generated configuration visible to the running service child.
 * In the normal jail this is its bound file, even after a host-side replacement.
 */
static int loaded_pool(struct management_ready *s, unsigned pid, const char *config)
{
	char path[512], *line = NULL, bus_name[128] = "";
	size_t size = 0;
	unsigned interfaces = 0, bridge = 0, ranges = 0;
	int invalid = 0, denied = 0;
	uint32_t mask = UINT32_MAX << (32 - s->prefix), network = s->address & mask;
	uint32_t broadcast = network | ~mask;
	if (strncmp(config, "/var/etc/dnsmasq.conf.", 22))
		return 0;
	int n = snprintf(path, sizeof(path), "/proc/%u/root%s", pid, config);
	if (n < 0 || (size_t)n >= sizeof(path))
		return 0;
	FILE *f = fopen(path, "re");
	if (!f)
		return 0;
	while (getline(&line, &size, f) >= 0) {
		line[strcspn(line, "\r\n")] = 0;
		if (!strncmp(line, "interface=", 10)) {
			interfaces++;
			bridge |= !strcmp(line + 10, "br-usb");
		} else if (!strncmp(line, "no-dhcp-interface=", 18) ||
			   !strncmp(line, "except-interface=", 17)) {
			const char *value = strchr(line, '=') + 1;
			denied |= !strcmp(value, "br-usb") || !strcmp(value, "*");
		} else if (!strcmp(line, "dhcp-ignore=usbmgmt") ||
			   !strcmp(line, "dhcp-ignore=tag:usbmgmt")) {
			denied = 1;
		} else if (!strncmp(line, "dhcp-range=set:usbmgmt,", 23)) {
			char *cursor = line + 23, *part[4];
			for (unsigned i = 0; i < 4; i++)
				part[i] = strsep(&cursor, ",");
			uint32_t low, high, actual_mask;
			ranges++;
			if (!part[0] || !part[1] || !part[2] || !part[3] || !*part[3] || cursor ||
			    ipv4(part[0], &low) || ipv4(part[1], &high) ||
			    ipv4(part[2], &actual_mask) || actual_mask != mask || low <= network ||
			    high >= broadcast || low > high ||
			    (s->address >= low && s->address <= high))
				invalid = 1;
		} else if (!strcmp(line, "enable-ubus")) {
			strcpy(bus_name, "dnsmasq");
		} else if (!strncmp(line, "enable-ubus=", 12)) {
			if (strlen(line + 12) >= sizeof(bus_name))
				invalid = 1;
			else
				strcpy(bus_name, line + 12);
		}
	}
	invalid |= ferror(f);
	free(line);
	fclose(f);
	if (ranges != 1 || invalid || denied || (interfaces && !bridge) || !*bus_name)
		return 0;
	strcpy(s->bus_name, bus_name);
	return 1;
}

/* procd's PID is ujail when sandboxed. Its standard jail pidfile names
 * the child in the host PID namespace; dnsmasq's own pidfile contains 1.
 */
static unsigned instance_pid(struct blob_attr *instance, unsigned supervisor)
{
	struct blob_attr *jail = field(instance, "jail", BLOBMSG_TYPE_TABLE);
	if (!jail)
		return supervisor;
	struct blob_attr *path = field(jail, "pidfile", BLOBMSG_TYPE_STRING);
	if (!path)
		return 0;
	FILE *f = fopen(blobmsg_get_string(path), "re");
	unsigned pid = 0;
	if (!f)
		return 0;
	if (fscanf(f, "%u", &pid) != 1)
		pid = 0;
	fclose(f);
	return pid > 1 ? pid : 0;
}

static void service_reply(struct ubus_request *request, int type, struct blob_attr *message)
{
	struct management_ready *s = request->priv;
	(void)type;
	for (unsigned which = 0; which < 2; which++) {
		struct blob_attr *service =
		    field(message, which ? "dnsmasq" : "log", BLOBMSG_TYPE_TABLE);
		struct blob_attr *instances = field(service, "instances", BLOBMSG_TYPE_TABLE),
				 *instance;
		size_t rem;
		if (!instances)
			continue;
		blobmsg_for_each_attr(instance, instances, rem)
		{
			struct blob_attr *running = field(instance, "running", BLOBMSG_TYPE_BOOL);
			struct blob_attr *pid = field(instance, "pid", BLOBMSG_TYPE_INT32);
			if (!running || !blobmsg_get_bool(running) || !pid ||
			    blobmsg_get_u32(pid) <= 1)
				continue;
			if (!which) {
				s->logd = 1;
				continue;
			}
			s->dnsmasq = 1;
			struct blob_attr *command = field(instance, "command", BLOBMSG_TYPE_ARRAY),
					 *arg;
			size_t left;
			int next = 0;
			if (!command)
				continue;
			blobmsg_for_each_attr(arg, command, left)
			{
				if (blobmsg_type(arg) != BLOBMSG_TYPE_STRING)
					break;
				const char *value = blobmsg_get_string(arg);
				if (next) {
					unsigned child =
					    instance_pid(instance, blobmsg_get_u32(pid));
					if (child && loaded_pool(s, child, value))
						s->pool = 1;
					break;
				}
				next = !strcmp(value, "-C");
			}
		}
	}
}

static void metrics_reply(struct ubus_request *request, int type, struct blob_attr *message)
{
	struct management_ready *s = request->priv;
	(void)type;
	/* A zero DHCP count is valid: a connected host is not an acceptance gate. */
	s->metrics = field(message, "dhcp_discover", BLOBMSG_TYPE_INT32) != NULL;
}

static int invoke(struct ubus_context *ctx, const char *object, const char *method,
		  ubus_data_handler_t callback, struct management_ready *s)
{
	uint32_t id;
	int ret = ubus_lookup_id(ctx, object, &id);
	return ret ? ret : ubus_invoke(ctx, id, method, NULL, callback, s, 500);
}

int udx_management_ready(const char **reason)
{
	struct management_ready s = {};
	struct stat st;
	struct ubus_context *ctx;
	int ret = -1;
	*reason = "management-ip-not-ready";
	/* libubus object lookup has no timeout argument. Bound the entire helper,
	 * including lookup; boot acceptance retains its existing 3s child budget.
	 */
	alarm(2);
	if (configured_address(&s.address))
		goto out;
	ctx = ubus_connect(NULL);
	if (!ctx)
		goto out;
	if (invoke(ctx, "network.interface.usbmgmt", "status", network_reply, &s) || !s.network)
		goto done;
	*reason = "services-not-ready";
	if (invoke(ctx, "service", "list", service_reply, &s))
		goto done;
	*reason = "logd-not-ready";
	if (!s.logd || stat("/dev/log", &st) || !S_ISSOCK(st.st_mode))
		goto done;
	*reason = "dnsmasq-not-running";
	if (!s.dnsmasq)
		goto done;
	*reason = "management-pool-not-ready";
	if (!s.pool)
		goto done;
	*reason = "dnsmasq-not-responsive";
	if (invoke(ctx, s.bus_name, "metrics", metrics_reply, &s) || !s.metrics)
		goto done;
	*reason = "ready";
	ret = 0;
done:
	ubus_free(ctx);
out:
	alarm(0);
	return ret;
}
