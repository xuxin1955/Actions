/* SPDX-License-Identifier: GPL-2.0-only */
#include "cellular.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
	const char *command = argc > 1 ? argv[1] : "status";
	if (argc > 2 || (strcmp(command, "status") && strcmp(command, "refresh") &&
			 strcmp(command, "connect") && strcmp(command, "disconnect")))
		return 64;
	J *args = json_object_new_object();
	if (!strcmp(command, "connect"))
		boolean(args, "automatic", false);
	J *reply = cell_bus_call(command, args, 3000);
	json_object_put(args);
	if (!reply) {
		fprintf(stderr, "%s\n", cell_error);
		return 1;
	}
	if (!json_object_get_boolean(get(reply, "accepted")) || !strcmp(command, "status")) {
		puts(json_object_to_json_string_ext(reply, JSON_C_TO_STRING_PLAIN |
							       JSON_C_TO_STRING_NOSLASHESCAPE));
		int status = json_object_get_boolean(get(reply, "accepted")) ? 0 : 1;
		json_object_put(reply);
		return status;
	}
	double end = mono() + 230;
	int64_t id = json_object_get_int64(get(reply, "operation_id"));
	J *reference = json_object_new_object();
	integer(reference, "id", id);
	string(reference, "boot_id", str(reply, "boot_id"));
	string(reference, "broker_id", str(reply, "broker_id"));
	while (mono() < end) {
		J *row = cell_bus_call("operation_status", reference, 3000);
		if (row && !json_object_get_boolean(get(row, "accepted"))) {
			errorf(CELL_UNCERTAIN, "%s",
			       str(row, "error") ? str(row, "error")
						 : "Operation receipt unavailable");
			json_object_put(row);
			row = NULL;
		}
		if (!row) {
			json_object_put(reference);
			fprintf(stderr, "%s; request was not replayed\n", cell_error);
			json_object_put(reply);
			return 1;
		}
		if (json_object_get_int64(get(row, "id")) == id &&
		    text_equal(str(row, "broker_id"), str(reply, "broker_id")) &&
		    text_equal(str(row, "boot_id"), str(reply, "boot_id")) &&
		    (text_equal(str(row, "state"), "complete") ||
		     text_equal(str(row, "state"), "failed") ||
		     text_equal(str(row, "state"), "uncertain"))) {
			puts(json_object_to_json_string_ext(
			    row, JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE));
			int status = text_equal(str(row, "state"), "complete") ? 0 : 1;
			json_object_put(row);
			json_object_put(reference);
			json_object_put(reply);
			return status;
		}
		json_object_put(row);
		delay_ms(100);
	}
	json_object_put(reference);
	json_object_put(reply);
	fprintf(stderr, "Operation timeout: request was not replayed\n");
	return 1;
}
