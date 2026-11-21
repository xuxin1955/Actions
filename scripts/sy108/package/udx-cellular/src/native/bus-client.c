/* SPDX-License-Identifier: GPL-2.0-only */
#include "common.h"
#include <errno.h>
#include <libubox/blobmsg_json.h>
#include <libubus.h>
#include <stdlib.h>
#include <string.h>

static void response(struct ubus_request *request, int type, struct blob_attr *msg)
{
	J **result = request->priv;
	(void)type;
	if (!msg)
		return;
	char *text = blobmsg_format_json(msg, true);
	if (text) {
		json_object_put(*result);
		*result = parse_json(text, strlen(text));
		free(text);
	}
}

J *cell_bus_call(const char *method, J *args, unsigned timeout_ms)
{
	struct ubus_context *ctx = ubus_connect(getenv("UBUS_SOCKET"));
	if (!ctx) {
		errno = ECONNREFUSED;
		errorf(CELL_SYSTEM, "Cannot connect to ubus");
		return NULL;
	}
	uint32_t id;
	J *result = NULL;
	struct blob_buf b = {0};
	blob_buf_init(&b, 0);
	int ret = ubus_lookup_id(ctx, "udx.cellular", &id);
	if (!ret && args &&
	    !blobmsg_add_json_from_string(
		&b, json_object_to_json_string_ext(args, JSON_C_TO_STRING_PLAIN)))
		ret = UBUS_STATUS_INVALID_ARGUMENT;
	if (!ret)
		ret = ubus_invoke(ctx, id, method, b.head, response, &result, timeout_ms);
	blob_buf_free(&b);
	ubus_free(ctx);
	if (ret) {
		json_object_put(result);
		errno = ret == UBUS_STATUS_NOT_FOUND ? ENOENT
			: ret == UBUS_STATUS_TIMEOUT ? ETIMEDOUT
						     : EIO;
		errorf(CELL_SYSTEM, "Cellular ubus %s: %s", method, ubus_strerror(ret));
		return NULL;
	}
	if (!result) {
		errno = EPROTO;
		errorf(CELL_SYSTEM, "Cellular ubus response missing");
	}
	return result;
}
