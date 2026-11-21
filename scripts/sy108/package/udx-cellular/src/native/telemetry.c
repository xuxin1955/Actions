/* SPDX-License-Identifier: GPL-2.0-only */
/* Low-priority queries share the sole AT owner. */
#include "cellular.h"
#include <string.h>
#include <stdio.h>
#include <unistd.h>

static const char *operations[] = {"modem_info",	"signal_refresh",   "serving_refresh",
				   "neighbors_refresh", "advanced_refresh", "radio_options",
				   "qos_refresh",	"apn_templates"};
static const unsigned intervals[] = {3600, 10, 15, 60, 120, 300, 300, 120};
_Static_assert(sizeof(operations) / sizeof(*operations) == CELL_TELEMETRY_TASKS,
	       "telemetry task count");
_Static_assert(sizeof(intervals) / sizeof(*intervals) == CELL_TELEMETRY_TASKS,
	       "telemetry interval count");

static bool paused(void)
{
	return !access(CELL_RUN "/telemetry-paused", F_OK);
}

int telemetry_publish(struct controller *c)
{
	J *state = get(c->status, "telemetry");
	bool changed = !state || json_object_get_boolean(get(state, "enabled")) == paused();
	if (!state) {
		state = json_object_new_object();
		json_object_object_add(c->status, "telemetry", state);
		json_object_object_add(state, "queries", json_object_new_object());
	}
	boolean(state, "enabled", !paused());
	string(state, "transport", "single-owner-command-boundary-priority");
	return changed ? controller_publish(c) : 0;
}

int telemetry_step(struct controller *c)
{
	if (telemetry_publish(c) < 0)
		return -1;
	if (paused() || c->transport.poisoned ||
	    (c->foreground_pending && c->foreground_pending(c->foreground_context)))
		return 0;
	const char *sim = str(get(c->status, "sim"), "state");
	if (!sim)
		sim = "";
	if (!text_equal(sim, c->telemetry_sim)) {
		snprintf(c->telemetry_sim, sizeof(c->telemetry_sim), "%s", sim ? sim : "");
		c->telemetry_due[0] = 0;
		c->telemetry_due[5] = 0;
	}
	double now = mono();
	for (size_t i = 0; i < CELL_TELEMETRY_TASKS; i++) {
		if (now < c->telemetry_due[i])
			continue;
		J *args = json_object_new_object();
		c->background = true;
		c->background_deferred = false;
		double started = mono();
		J *result = controller_execute(c, operations[i], args);
		c->background = false;
		json_object_put(args);
		if (c->background_deferred) {
			json_object_put(result);
			c->telemetry_due[i] = mono() + 1;
			return 0;
		}
		J *queries = get(get(c->status, "telemetry"), "queries");
		J *previous = get(queries, operations[i]);
		J *row = previous ? jclone(previous) : json_object_new_object();
		real(row, "attempt_uptime", mono());
		real(row, "elapsed_ms", (mono() - started) * 1000);
		integer(row, "attempts", json_object_get_int64(get(row, "attempts")) + 1);
		integer(row, "interval_seconds", intervals[i]);
		bool partial =
		    result && ((get(result, "info_errors") &&
				json_object_object_length(get(result, "info_errors"))) ||
			       (get(result, "cell_query_errors") &&
				json_object_object_length(get(result, "cell_query_errors"))));
		if (result) {
			real(row, "updated_uptime", mono());
			string(row, "state", partial ? "partial" : "ready");
			string(row, "error", NULL);
			c->telemetry_failures[i] = 0;
			c->telemetry_due[i] = mono() + intervals[i];
		} else {
			unsigned failures = ++c->telemetry_failures[i];
			unsigned backoff = failures < 5 ? 15u << failures : 300;
			string(row, "state", get(row, "updated_uptime") ? "stale" : "error");
			string(row, "error", cell_error);
			c->telemetry_due[i] = mono() + backoff;
		}
		real(row, "next_uptime", c->telemetry_due[i]);
		json_object_object_add(queries, operations[i], row);
		json_object_put(result);
		int published = controller_publish(c);
		return c->transport.poisoned ? -1 : published;
	}
	return 0;
}
