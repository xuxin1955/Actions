/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include <libubus.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* Shared by explicit runtime actions and upgrade prepare. Ordinary init
 * scripts invoke their own service method and deliberately log/continue. */
int main(int argc, char **argv)
{
	(void)argv;
	if (geteuid() || argc != 1)
		return 64;
	struct ubus_context *bus = ubus_connect(getenv("UBUS_SOCKET"));
	if (!bus) {
		fputs("Cannot connect to ubus for service stop\n", stderr);
		return 1;
	}
	const char *objects[] = {"udx.cellular", "udx.cpnv"};
	int failed = 0;
	for (unsigned i = 0; i < 2; i++) {
		uint32_t id;
		int ret = ubus_lookup_id(bus, objects[i], &id);
		if (!ret)
			ret = ubus_invoke(bus, id, "quiesce", NULL, NULL, NULL, 12000);
		if (ret) {
			fprintf(stderr, "%s quiesce: %s\n", objects[i], ubus_strerror(ret));
			failed = 1;
		}
	}
	ubus_free(bus);
	return failed;
}
