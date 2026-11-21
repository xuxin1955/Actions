/* SPDX-License-Identifier: GPL-2.0-only */
#include "cellular.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool radio_enabled(struct controller *c)
{
	return !text_equal(str(c->config, "radio_enabled"), "0");
}

int radio_state(struct controller *c)
{
	J *row = controller_query(c, "AT+CFUN?", 10, "+CFUN:");
	if (!row)
		return -1;
	int64_t state = -1;
	bool valid =
	    json_object_array_length(row) == 1 &&
	    integer_text(json_object_get_string(json_object_array_get_idx(row, 0)), 0, 4, &state) &&
	    (state == 0 || state == 1 || state == 4);
	json_object_put(row);
	if (!valid)
		return errorf(CELL_INVALID, "Unsupported CFUN state");
	integer(c->status, "cfun", state);
	return (int)state;
}

int radio_command(struct controller *c, const char *wire, double timeout)
{
	char *raw = controller_at(c, wire, timeout);
	if (!raw)
		return -1;
	free(raw);
	return 0;
}

int radio_restore(struct controller *c, bool enabled, bool wanted)
{
	c->restore_radio = enabled && radio_enabled(c);
	c->restore_data = wanted;
	c->radio_restore_pending = true;
	if (c->transport.poisoned)
		return errorf(CELL_UNCERTAIN, "Radio restoration awaits AT recovery");
	int state = radio_state(c);
	if (state < 0)
		return -1;
	if (c->restore_radio) {
		if (controller_radio_on(c) < 0)
			return -1;
	} else if (state == 1) {
		if (radio_command(c, "AT+SFUN=5", 50) < 0)
			return -1;
		state = radio_state(c);
		if (state < 0)
			return -1;
		if (state == 1)
			return errorf(CELL_MODEM, "Radio-off readback mismatch");
	}
	c->want_data = c->restore_data;
	c->radio_restore_pending = false;
	return controller_publish(c);
}

int radio_begin(struct controller *c, struct radio_change *change)
{
	memset(change, 0, sizeof(*change));
	int state = radio_state(c);
	if (state < 0)
		return -1;
	change->active = true;
	change->enabled = state == 1 && radio_enabled(c);
	change->wanted = c->want_data;
	c->want_data = false;
	J *result = controller_disconnect(c, false);
	if (!result)
		return -1;
	json_object_put(result);
	if (state == 1 && radio_command(c, "AT+SFUN=5", 50) < 0)
		return -1;
	state = radio_state(c);
	if (state < 0)
		return -1;
	return state != 1 ? 0 : errorf(CELL_MODEM, "Radio did not go offline");
}

int radio_end(struct controller *c, struct radio_change *change, bool success)
{
	if (!change->active)
		return success ? 0 : -1;
	char saved[512];
	snprintf(saved, sizeof(saved), "%s", cell_error);
	enum cell_error_kind kind = cell_error_kind;
	int rc = radio_restore(c, change->enabled, change->wanted);
	if (!success) {
		errorf(kind, "%s", saved);
		return -1;
	}
	return rc;
}
