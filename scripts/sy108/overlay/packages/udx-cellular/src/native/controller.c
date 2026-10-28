/* SPDX-License-Identifier: GPL-2.0-only */
#include "cellular.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct operation {
	const char *name;
	unsigned family;
	const char *fields[8];
	size_t count;
};
static const struct operation operations[] = {
    {"imei_read", 8, {NULL}, 0},
    {"imei_write", 8, {"slot", "imei"}, 2},
    {"operator_catalog", 7, {NULL}, 0},
    {"operators_scan", 7, {NULL}, 0},
    {"operator_auto", 7, {NULL}, 0},
    {"operator_select", 7, {"plmn", "act"}, 2},
    {"advanced_refresh", 1, {NULL}, 0},
    {"at_debug", 6, {"command", "timeout"}, 2},
    {"apn", 0, {"apn", "pdp_type", "cid"}, 3},
    {"apn_select", 2, {"mode", "template_id"}, 2},
    {"apn_template_delete", 2, {"id"}, 1},
    {"apn_template_save",
     2,
     {"id", "name", "apn", "pdp_type", "username", "password", "auth_method"},
     7},
    {"apn_templates", 2, {NULL}, 0},
    {"band_lock", 1, {"lte", "nr"}, 2},
    {"cell_lock", 1, {"rat", "arfcn", "pci"}, 3},
    {"cells_refresh", 3, {NULL}, 0},
    {"serving_refresh", 3, {NULL}, 0},
    {"neighbors_refresh", 3, {NULL}, 0},
    {"signal_refresh", 3, {NULL}, 0},
    {"qos_refresh", 3, {NULL}, 0},
    {"connect", 0, {"automatic"}, 1},
    {"disconnect", 0, {NULL}, 0},
    {"modem_info", 3, {NULL}, 0},
    {"network_mode", 1, {"mode"}, 1},
    {"network_configure", 1, {"mode", "nr_capability"}, 2},
    {"network_preset", 1, {"mode"}, 1},
    {"nr_capability", 4, {"mode"}, 1},
    {"physical_slot", 4, {"slot"}, 1},
    {"policy", 0, {"automatic_connect", "roaming"}, 2},
    {"radio", 0, {"enabled"}, 1},
    {"radio_options", 4, {NULL}, 0},
    {"refresh", 0, {NULL}, 0},
    {"sms_delete_batch", 5, {"items"}, 1},
    {"sms_delete", 5, {"index", "pdu_sha256"}, 2},
    {"sms_delete_history", 5, {"folder", "id"}, 2},
    {"sms_history", 5, {NULL}, 0},
    {"sms_mark_read", 5, {"id"}, 1},
    {"sms_refresh", 5, {NULL}, 0},
    {"sms_send", 5, {"number", "text", "request_id"}, 3},
    {"sms_settings", 5, {"max_count", "max_sent_count", "direct_receive"}, 3},
    {"status", 0, {NULL}, 0},
    {"operation_status", 0, {"id", "boot_id", "broker_id"}, 3},
    {"unlock", 1, {"scope"}, 1}};

static const struct operation *lookup(const char *name)
{
	for (size_t i = 0; i < sizeof(operations) / sizeof(*operations); i++)
		if (text_equal(name, operations[i].name))
			return &operations[i];
	return NULL;
}

J *controller_operations(void)
{
	J *out = json_object_new_array();
	for (size_t i = 0; i < sizeof(operations) / sizeof(*operations); i++)
		json_object_array_add(out, json_object_new_string(operations[i].name));
	return out;
}

int controller_validate(const char *name, J *args)
{
	const struct operation *op = lookup(name);
	if (!op)
		return errorf(CELL_INVALID, "Unknown cellular operation");
	if (!strcmp(name, "apn")) {
		const char *const extra[] = {"apn",	 "pdp_type", "cid",
					     "username", "password", "auth_method"};
		if (!keys(args, op->fields, 3) && !keys(args, extra, 6))
			return errorf(CELL_INVALID, "Invalid APN arguments");
		J *p = json_object_new_object();
		for (size_t i = 0; i < 3; i++)
			json_object_object_add(p, op->fields[i],
					       json_object_get(get(args, op->fields[i])));
		int rc = profile_validate(p);
		json_object_put(p);
		if (rc < 0)
			return -1;
		return get(args, "auth_method") ? apn_auth_validate(args) : 0;
	}
	if (!strcmp(name, "unlock") && text_equal(str(args, "scope"), "cells")) {
		const char *fields[] = {"scope", "rat"};
		if (!keys(args, fields, 2))
			return errorf(CELL_INVALID, "Invalid cell unlock arguments");
		return advanced_validate(name, args);
	}
	if (!keys(args, op->fields, op->count))
		return errorf(CELL_INVALID, "Invalid cellular arguments");
	if (!strcmp(name, "operation_status")) {
		if (!json_object_is_type(get(args, "id"), json_type_int) ||
		    json_object_get_int64(get(args, "id")) < 1 || !str(args, "boot_id") ||
		    strlen(str(args, "boot_id")) != 36 || !hex_string(str(args, "broker_id"), 32))
			return errorf(CELL_INVALID, "Invalid operation receipt reference");
		return 0;
	}
	if (op->family == 8)
		return identity_validate(name, args);
	if (op->family == 7)
		return operator_validate(name, args);
	if (op->family == 6)
		return debug_validate(args);
	if (op->family == 1)
		return advanced_validate(name, args);
	if (op->family == 2)
		return apn_validate(name, args);
	if (op->family == 4)
		return mode_validate(name, args);
	if (!strcmp(name, "connect") || !strcmp(name, "radio") || !strcmp(name, "policy")) {
		for (size_t i = 0; i < op->count; i++)
			if (!json_object_is_type(get(args, op->fields[i]), json_type_boolean))
				return errorf(CELL_INVALID, "Expected boolean argument");
	}
	if (!strcmp(name, "sms_settings"))
		return sms_settings_validate(args);
	if (!strcmp(name, "sms_send")) {
		if (!hex_string(str(args, "request_id"), 32))
			return errorf(CELL_INVALID, "Invalid SMS request identifier");
		J *parts = sms_encode(str(args, "number"), str(args, "text"), 0);
		if (!parts)
			return -1;
		json_object_put(parts);
	}
	if (!strcmp(name, "sms_mark_read") || !strcmp(name, "sms_delete_history")) {
		const char *folder = !strcmp(name, "sms_mark_read") ? "inbox" : str(args, "folder");
		if ((!text_equal(folder, "inbox") && !text_equal(folder, "sent")) ||
		    !hex_string(str(args, "id"), text_equal(folder, "inbox") ? 64 : 32))
			return errorf(CELL_INVALID, "Invalid SMS history selection");
	}
	if (!strcmp(name, "sms_delete_batch")) {
		J *items = get(args, "items");
		if (!json_object_is_type(items, json_type_array) ||
		    !json_object_array_length(items) || json_object_array_length(items) > 50)
			return errorf(CELL_INVALID, "Select 1 to 50 SMS records");
		for (size_t i = 0; i < json_object_array_length(items); i++) {
			J *item = json_object_array_get_idx(items, i);
			if (controller_validate(
				get(item, "index") ? "sms_delete" : "sms_delete_history", item) < 0)
				return -1;
			for (size_t j = 0; j < i; j++)
				if (json_object_equal(item, json_object_array_get_idx(items, j)))
					return errorf(CELL_INVALID, "Duplicate SMS selection");
		}
	}
	if (!strcmp(name, "sms_delete")) {
		int64_t index = json_object_get_int64(get(args, "index"));
		if (!json_object_is_type(get(args, "index"), json_type_int) || index < 1 ||
		    index > 65535 || !hex_string(str(args, "pdu_sha256"), 64))
			return errorf(CELL_INVALID, "Invalid stored SMS selection");
	}
	return 0;
}

J *controller_config(void)
{
	const char *names[] = {
	    "apn",	    "pdp_type",	     "cid",	     "automatic_connect", "roaming",
	    "username",	    "password",	     "auth_method",  "apn_mode",	  "apn_template_id",
	    "network_mode", "nr_capability", "radio_enabled"};
	const char *defaults[] = {"",	  "IPV4V6", "1", "1", "0",	"", "",
				  "none", "auto",   "0", "",  "sa_nsa", "1"};
	J *sections = cell_config_sections();
	if (!sections)
		return NULL;
	J *row = get(sections, "modem"), *out = json_object_new_object();
	for (size_t i = 0; i < sizeof(names) / sizeof(*names); i++)
		string(out, names[i], str(row, names[i]) ? str(row, names[i]) : defaults[i]);
	json_object_put(sections);
	if (text_equal(str(out, "apn_mode"), "auto"))
		string(out, "apn", "");
	if (!text_equal(str(out, "cid"), "1"))
		goto invalid;
	J *p = json_object_new_object();
	string(p, "apn", str(out, "apn"));
	string(p, "pdp_type", str(out, "pdp_type"));
	integer(p, "cid", 1);
	int rc = profile_validate(p);
	json_object_put(p);
	if (rc < 0)
		goto fail;
	const char *booleans[] = {"automatic_connect", "roaming", "radio_enabled"};
	for (size_t i = 0; i < 3; i++)
		if (!text_equal(str(out, booleans[i]), "0") &&
		    !text_equal(str(out, booleans[i]), "1"))
			goto invalid;
	if (apn_auth_validate(out) < 0)
		goto fail;
	if (!text_equal(str(out, "apn_mode"), "auto") &&
	    !text_equal(str(out, "apn_mode"), "manual"))
		goto invalid;
	if (!integer_text(str(out, "apn_template_id"), 0, INT_MAX - 1, NULL))
		goto invalid;
	if (*str(out, "network_mode")) {
		J *v = json_object_new_object();
		string(v, "mode", str(out, "network_mode"));
		rc = advanced_validate("network_mode", v);
		json_object_put(v);
		if (rc < 0)
			goto fail;
	}
	return out;
invalid:
	errorf(CELL_INVALID, "Invalid cellular configuration");
fail:
	json_object_put(out);
	return NULL;
}

int controller_init(struct controller *c)
{
	memset(c, 0, sizeof(*c));
	c->transport.fd = c->transport.urc_fd = c->transport.lock_fd = -1;
	c->config = controller_config();
	if (!c->config)
		return -1;
	c->status = json_object_new_object();
	if (transport_init(&c->transport) < 0)
		return -1;
	return 0;
}

void controller_destroy(struct controller *c)
{
	transport_close(&c->transport);
	json_object_put(c->fallback_profile);
	json_object_put(c->config);
	json_object_put(c->status);
	json_object_put(c->data);
	c->config = c->status = c->data = NULL;
}

J *controller_profile(struct controller *c)
{
	J *p = json_object_new_object();
	J *profile = c->fallback_profile ? c->fallback_profile : c->config;
	string(p, "apn", str(profile, "apn"));
	string(p, "pdp_type", str(profile, "pdp_type"));
	integer(p, "cid", 1);
	return p;
}

static bool registered(struct controller *c)
{
	return json_object_get_boolean(get(get(c->status, "registration"), "registered")) ||
	       json_object_get_boolean(get(get(c->status, "nr_registration"), "registered"));
}

static bool roaming(struct controller *c)
{
	return json_object_get_boolean(get(get(c->status, "registration"), "roaming")) ||
	       json_object_get_boolean(get(get(c->status, "nr_registration"), "roaming"));
}

int controller_publish(struct controller *c)
{
	const char *state =
	    c->transport.poisoned					? "uncertain"
	    : c->data							? "pdp_active"
	    : !c->want_data						? "idle"
	    : !text_equal(str(get(c->status, "sim"), "state"), "READY") ? "waiting_sim"
	    : json_object_get_int(get(c->status, "cfun")) != 1		? "waiting_radio"
	    : !registered(c)						? "waiting_registration"
	    : json_object_get_int(get(c->status, "attached")) != 1	? "waiting_attachment"
									: "connecting";
	string(c->status, "boot_id", c->transport.boot);
	real(c->status, "uptime", mono());
	string(c->status, "state", state);
	string(c->status, "last_error", *c->error ? c->error : NULL);
	json_object_object_add(c->status, "profile", controller_profile(c));
	boolean(c->status, "automatic_connect",
		text_equal(str(c->config, "automatic_connect"), "1"));
	json_object_object_add(c->status, "data_session", json_object_get(c->data));
	boolean(c->status, "radio_enabled", radio_enabled(c));
	boolean(c->status, "data_requested", c->want_data);
	boolean(c->status, "radio_restore_pending", c->radio_restore_pending);
	boolean(c->status, "roaming_allowed", text_equal(str(c->config, "roaming"), "1"));
	boolean(c->status, "roaming", roaming(c));
	json_object_object_add(c->status, "auth", apn_public_auth(c->config));
	string(c->status, "apn_mode", str(c->config, "apn_mode"));
	json_object_object_add(c->status, "supported_operations", controller_operations());
	return c->publish_state ? c->publish_state(c->service_context, c->status) : 0;
}

int controller_set_data(struct controller *c, J *session)
{
	J *old = c->data;
	c->data = json_object_get(session);
	json_object_put(old);
	return controller_publish(c);
}

char *controller_at(struct controller *c, const char *command, double seconds)
{
	if (c->background && c->foreground_pending &&
	    c->foreground_pending(c->foreground_context)) {
		c->background_deferred = true;
		errorf(CELL_SYSTEM, "Background query yielded to a pending operation");
		return NULL;
	}
	if (seconds <= 0) {
		seconds = 50;
		const char *prefixes[] = {"AT+CGDATA=",	 "AT+CGACT=0",	"AT+CGACT?",
					  "AT+CGDCONT?", "AT+CGDCONT=", "AT+CPIN?",
					  "AT+CEREG",	 "AT+CGEREP",	"AT+CGEQREQ"};
		double limits[] = {120, 50, 5, 5, 5, 8, 15, 5, 5};
		for (size_t i = 0; i < 9; i++)
			if (!strncmp(command, prefixes[i], strlen(prefixes[i]))) {
				seconds = limits[i];
				break;
			}
	}
	return transport_command(&c->transport, command, seconds, NULL, false);
}

J *controller_query(struct controller *c, const char *command, double seconds, const char *prefix)
{
	char *raw = controller_at(c, command, seconds);
	if (!raw)
		return NULL;
	J *row = at_one(raw, prefix);
	free(raw);
	return row;
}

static int query_int(struct controller *c, const char *command, double timeout, const char *prefix,
		     int64_t max, int *value)
{
	J *row = controller_query(c, command, timeout, prefix);
	if (!row)
		return -1;
	int64_t n;
	bool valid =
	    json_object_array_length(row) >= 1 &&
	    integer_text(json_object_get_string(json_object_array_get_idx(row, 0)), 0, max, &n);
	json_object_put(row);
	if (!valid)
		return errorf(CELL_INVALID, "Invalid numeric AT readback");
	*value = (int)n;
	return 0;
}

static int command(struct controller *c, const char *text, double timeout)
{
	char *raw = controller_at(c, text, timeout);
	if (!raw)
		return -1;
	free(raw);
	return 0;
}

static int network_release(struct controller *c, const char *id)
{
	if (!id)
		return 0;
	return c->retire_network ? c->retire_network(c->service_context) : 0;
}

J *controller_disconnect(struct controller *c, bool quiesce)
{
	char *old = c->data && str(c->data, "id") ? strdup(str(c->data, "id")) : NULL;
	int rc = controller_set_data(c, NULL);
	if (!rc)
		rc = network_release(c, old);
	free(old);
	if (rc < 0)
		return NULL;
	string(c->status, "pdp_state", "unknown");
	char at_error[512] = "";
	if (!c->transport.poisoned && !atomic_load(&c->transport.stopping)) {
		char *raw = controller_at(c, "AT+CGACT?", 5);
		int active = raw ? bearer_active(raw) : -1;
		free(raw);
		rc = active;
		if (active > 0)
			rc = command(c, "AT+CGACT=0,1", 50);
		else if (!active)
			string(c->status, "pdp_state", "inactive");
		if (rc < 0) {
			/* Quiesce retires AP networking and closes AT ownership. A dead
			 * CP cannot confirm PDP teardown; report it as unknown while the
			 * independent native shutdown still verifies its NV drain. */
			if (!quiesce)
				return NULL;
			snprintf(at_error, sizeof(at_error), "%s", cell_error);
		}
	}
	if (controller_publish(c) < 0)
		return NULL;
	J *result = json_object_new_object();
	boolean(result, "network_retired", true);
	string(result, "pdp_state", str(c->status, "pdp_state"));
	if (*at_error)
		string(result, "at_error", at_error);
	return result;
}

char *controller_sim(struct controller *c)
{
	J *row = controller_query(c, "AT+CPIN?", 8, "+CPIN:");
	if (!row) {
		if (cell_error_kind == CELL_MODEM || cell_error_kind == CELL_INVALID) {
			string(c->status, "sim_error", cell_error);
			return strdup("NOT_READY");
		}
		return NULL;
	}
	const char *state = json_object_get_string(json_object_array_get_idx(row, 0));
	char *result = state ? strdup(state) : NULL;
	json_object_put(row);
	if (!result)
		errorf(CELL_INVALID, "SIM state is missing");
	return result;
}

static int sim_refresh(struct controller *c)
{
	char *state = controller_sim(c);
	if (!state)
		return -1;
	const char *previous = str(get(c->status, "sim"), "state");
	if (!text_equal(previous, state)) {
		c->fallback_attempted = false;
		c->fallback_index = 0;
		c->fallback_tried[0] = 0;
		json_object_put(c->fallback_profile);
		c->fallback_profile = NULL;
		json_object_object_del(c->status, "automatic_apn");
		string(c->status, "iccid", NULL);
		string(c->status, "imsi", NULL);
		json_object_object_del(c->status, "modem_info_uptime");
		c->telemetry_due[0] = 0;
	}
	J *row = json_object_new_object();
	string(row, "state", state);
	json_object_object_add(c->status, "sim", row);
	free(state);
	return 0;
}

int controller_radio_on(struct controller *c)
{
	if (!radio_enabled(c))
		return errorf(CELL_INVALID, "Radio is disabled by configuration");
	int cfun;
	if (query_int(c, "AT+CFUN?", 50, "+CFUN:", INT_MAX, &cfun) < 0)
		return -1;
	integer(c->status, "cfun", cfun);
	if (cfun == 1)
		return 0;
	if (cfun != 0 && cfun != 4)
		return errorf(CELL_INVALID, "Unsupported current CFUN state");
	c->ims_initialized = false;
	if (command(c, "AT+SAUTOATT=1", 50) < 0)
		return -1;
	if (command(c, "AT+SFUN=4", 180) < 0) {
		if (cell_error_kind != CELL_MODEM && cell_error_kind != CELL_INVALID)
			return -1;
		if (query_int(c, "AT+CFUN?", 50, "+CFUN:", INT_MAX, &cfun) < 0)
			return -1;
		if (cfun != 1)
			return errorf(CELL_MODEM, "Radio enable rejected");
	}
	if (query_int(c, "AT+CFUN?", 50, "+CFUN:", INT_MAX, &cfun) < 0)
		return -1;
	integer(c->status, "cfun", cfun);
	return cfun == 1 ? 0 : errorf(CELL_MODEM, "Radio enable readback failed");
}

static int initialize(struct controller *c)
{
	if (c->initialized)
		return 0;
	const char *commands[] = {"ATE0Q0V1",	 "AT+CMEE=1",	  "AT+CEREG=2",
				  "AT+C5GREG=2", "AT+CGEREP=1,0", "AT+CGPIAF=1"};
	for (size_t i = 0; i < 6; i++)
		if (command(c, commands[i], 15) < 0) {
			if (i != 3 ||
			    (cell_error_kind != CELL_MODEM && cell_error_kind != CELL_INVALID))
				return -1;
		}
	if (*str(c->config, "network_mode")) {
		struct radio_change change;
		J *result = NULL, *nr = NULL;
		bool ok = radio_begin(c, &change) == 0;
		if (ok)
			ok = advanced_set_mode(c, str(c->config, "network_mode"), &result) == 0;
		if (ok) {
			nr = mode_set_nr(c, str(c->config, "nr_capability"));
			ok = nr != NULL;
		}
		int rc = radio_end(c, &change, ok);
		json_object_put(result);
		json_object_put(nr);
		if (rc < 0)
			return -1;
	}
	if (sms_configure(c, NULL) < 0) {
		if (c->transport.poisoned)
			return -1;
		string(c->status, "sms_receive_error", cell_error);
	} else
		string(c->status, "sms_receive_error", NULL);
	c->initialized = true;
	return 0;
}

J *registration(const char *raw, const char *prefix)
{
	J *rows = at_fields(raw, prefix), *selected = NULL;
	if (!rows)
		return NULL;
	for (size_t i = 0; i < json_object_array_length(rows); i++) {
		J *r = json_object_array_get_idx(rows, i);
		if (json_object_array_length(r) >= 2 &&
		    integer_text(json_object_get_string(json_object_array_get_idx(r, 0)), 0,
				 INT_MAX, NULL) &&
		    integer_text(json_object_get_string(json_object_array_get_idx(r, 1)), 0,
				 INT_MAX, NULL)) {
			if (selected) {
				selected = NULL;
				break;
			}
			selected = r;
		}
	}
	if (!selected) {
		json_object_put(rows);
		errorf(CELL_INVALID, "Missing or ambiguous registration query response");
		return NULL;
	}
	int64_t code;
	integer_text(json_object_get_string(json_object_array_get_idx(selected, 1)), 0, INT_MAX,
		     &code);
	J *out = json_object_new_object();
	integer(out, "status", code);
	boolean(out, "registered", code == 1 || code == 5);
	boolean(out, "roaming", code == 5);
	if (json_object_array_length(selected) > 4) {
		int64_t act;
		if (!integer_text(json_object_get_string(json_object_array_get_idx(selected, 4)), 0,
				  INT_MAX, &act)) {
			json_object_put(out);
			json_object_put(rows);
			errorf(CELL_INVALID, "Invalid registration access technology");
			return NULL;
		}
		string(out, "tac", json_object_get_string(json_object_array_get_idx(selected, 2)));
		string(out, "cell_id",
		       json_object_get_string(json_object_array_get_idx(selected, 3)));
		integer(out, "act", act);
	}
	json_object_put(rows);
	return out;
}

static J *read_registration(struct controller *c)
{
	char *raw = controller_at(c, "AT+CEREG?", 15);
	if (!raw)
		return NULL;
	J *lte = registration(raw, "+CEREG:");
	free(raw);
	if (!lte)
		return NULL;
	raw = controller_at(c, "AT+C5GREG?", 50);
	J *nr = raw ? registration(raw, "+C5GREG:") : NULL;
	free(raw);
	if (!nr) {
		if (cell_error_kind != CELL_MODEM && cell_error_kind != CELL_INVALID) {
			json_object_put(lte);
			return NULL;
		}
		nr = json_object_new_object();
		boolean(nr, "registered", false);
		string(nr, "error", cell_error);
	}
	json_object_object_add(c->status, "registration", lte);
	json_object_object_add(c->status, "nr_registration", nr);
	return json_object_get(json_object_get_boolean(get(nr, "registered")) ? nr : lte);
}

static J *waiting(struct controller *c, const char *reason, J *reg)
{
	if (controller_set_data(c, NULL) < 0)
		return NULL;
	J *out = json_object_new_object();
	boolean(out, "connected", false);
	string(out, "waiting_for", reason);
	if (reg)
		json_object_object_add(out, "registration", json_object_get(reg));
	return out;
}

static J *connected(struct controller *c)
{
	J *out = json_object_new_object();
	boolean(out, "connected", true);
	json_object_object_add(out, "session", json_object_get(c->data));
	return out;
}

J *controller_connect(struct controller *c, unsigned wait_seconds)
{
	if (!radio_enabled(c))
		return waiting(c, "radio_disabled", NULL);
	if (transport_admit(&c->transport) < 0 || initialize(c) < 0 || sim_refresh(c) < 0)
		return NULL;
	const char *sim = str(get(c->status, "sim"), "state");
	if (!text_equal(sim, "READY"))
		return waiting(c, sim, NULL);
	if (controller_radio_on(c) < 0)
		return NULL;
	double end = mono() + wait_seconds;
	J *reg = NULL;
	while (true) {
		json_object_put(reg);
		reg = read_registration(c);
		if (!reg)
			return NULL;
		if (json_object_get_boolean(get(reg, "registered")))
			break;
		if (mono() >= end) {
			J *out = waiting(c, "registration", reg);
			json_object_put(reg);
			return out;
		}
		delay_ms(2000);
	}
	if (!c->ims_initialized) {
		if (ims_initialize(c) < 0) {
			json_object_put(reg);
			return NULL;
		}
		c->ims_initialized = true;
	}
	bool roaming_blocked = json_object_get_boolean(get(reg, "roaming")) &&
			       !text_equal(str(c->config, "roaming"), "1");
	json_object_put(reg);
	if (roaming_blocked)
		return waiting(c, "roaming_allowed", NULL);
	int attached;
	if (query_int(c, "AT+CGATT?", 50, "+CGATT:", 1, &attached) < 0)
		return NULL;
	integer(c->status, "attached", attached);
	if (attached != 1)
		return waiting(c, "packet_attachment", NULL);
	J *wanted = controller_profile(c);
	char *raw = controller_at(c, "AT+CGACT?", 5);
	int active = raw ? bearer_active(raw) : -1;
	free(raw);
	if (active < 0) {
		json_object_put(wanted);
		return NULL;
	}
	if (c->data && active && json_object_equal(get(c->data, "profile"), wanted)) {
		json_object_put(wanted);
		return connected(c);
	}
	J *session = NULL;
	if (active) {
		raw = controller_at(c, "AT+SPIPCONTEXT?", 10);
		if (!raw)
			goto fail;
		J *context = bearer_default(raw, wanted);
		free(raw);
		if (!context)
			goto fail;
		json_object_object_add(c->status, "default_bearer", context);
	} else {
		/* Auto mode with no explicit APN: resolve the Internet profile
		 * from the catalogue before the first attach.  Without this the
		 * modem is asked to activate an empty APN (CME ERROR 30) and the
		 * post-failure fallback below becomes the only recovery path,
		 * which stays unreachable whenever the failure is not classed as
		 * a modem rejection.
		 *
		 * The candidate list follows the network the modem is camped on
		 * and only then the SIM's home operator.  A Broadnet SIM that
		 * roams onto a Unicom cell has to use the Unicom profile, and a
		 * fixed home APN is exactly what left the router unable to come
		 * online while it reported "China Unicom". */
		const char *auto_apn = str(c->config, "apn");
		if (text_equal(str(c->config, "apn_mode"), "auto") &&
		    (!auto_apn || !*auto_apn) && operator_next_candidate(c) > 0) {
			json_object_put(wanted);
			wanted = controller_profile(c);
		}
		if (apn_configure(c) < 0)
			goto fail;
	}
	raw = controller_at(c, "AT+CGDATA=\"M-ETHER\",1", 120);
	/* Walk the remaining catalogue candidates when the activation was
	 * rejected by the modem.  Each candidate is a profile of the same
	 * operator family, so retrying stays within the roaming agreement of
	 * the SIM's owner. */
	while (!raw && !active && cell_error_kind == CELL_MODEM && !c->transport.poisoned &&
	       text_equal(str(c->config, "apn_mode"), "auto")) {
		char saved[512];
		snprintf(saved, sizeof(saved), "%s", cell_error);
		enum cell_error_kind kind = cell_error_kind;
		if (operator_next_candidate(c) <= 0) {
			errorf(kind, "%s", saved);
			break;
		}
		json_object_put(wanted);
		wanted = controller_profile(c);
		if (apn_configure(c) < 0)
			goto fail;
		raw = controller_at(c, "AT+CGDATA=\"M-ETHER\",1", 120);
		if (!raw)
			errorf(kind, "%s", saved);
	}
	if (!raw)
		goto fail;
	bool found = false;
	char *save = NULL;
	for (char *s = strtok_r(raw, " \r\n\t", &save); s; s = strtok_r(NULL, " \r\n\t", &save))
		if (!strcmp(s, "CONNECT"))
			found = true;
	free(raw);
	if (!found) {
		errorf(CELL_MODEM, "M-ETHER did not connect");
		goto fail;
	}
	raw = controller_at(c, "AT+CGACT?", 5);
	active = raw ? bearer_active(raw) : -1;
	free(raw);
	if (active != 1) {
		if (active == 0)
			errorf(CELL_MODEM, "CID activation readback mismatch");
		goto fail;
	}
	raw = controller_at(c, "AT+CGCONTRDP=1", 10);
	if (!raw)
		goto fail;
	session = parse_pdp(raw, wanted);
	free(raw);
	if (!session)
		goto fail;
	char id[33];
	if (random_hex(id, 16) < 0)
		goto fail;
	string(session, "id", id);
	string(session, "interface", "wwan0");
	json_object_object_add(session, "profile", json_object_get(wanted));
	real(session, "connected_uptime", mono());
	c->error[0] = 0;
	string(c->status, "pdp_state", "active");
	if (controller_set_data(c, session) < 0)
		goto fail;
	json_object_put(session);
	json_object_put(wanted);
	return connected(c);
fail:
	if (c->transport.poisoned)
		c->want_data = false;
	json_object_put(session);
	json_object_put(wanted);
	return NULL;
}

int controller_refresh(struct controller *c)
{
	if (transport_admit(&c->transport) < 0 || sim_refresh(c) < 0)
		return -1;
	J *reg = read_registration(c);
	if (!reg)
		return -1;
	bool registered_now = json_object_get_boolean(get(reg, "registered"));
	json_object_put(reg);
	J *op = controller_query(c, "AT+COPS?", 50, "+COPS:");
	if (!op)
		return -1;
	int64_t selection, act;
	size_t n = json_object_array_length(op);
	if (!integer_text(json_object_get_string(json_object_array_get_idx(op, 0)), 0, INT_MAX,
			  &selection) ||
	    (n > 3 && !integer_text(json_object_get_string(json_object_array_get_idx(op, 3)), 0,
				    INT_MAX, &act))) {
		json_object_put(op);
		return errorf(CELL_INVALID, "Invalid operator response");
	}
	const char *reported = n > 2 ? json_object_get_string(json_object_array_get_idx(op, 2)) : NULL;
	/* The module keeps the AT+COPS display format in its own non-volatile
	 * memory.  Any external AT client - the stock web UI, a manual AT
	 * terminal, a third party tool - can switch it to the long-name format,
	 * which hides the digits and makes the UI report the wrong carrier.  A
	 * Broadnet SIM then showed up as "China Unicom" and never came online.
	 * Re-assert the numeric format whenever the reply is not a PLMN. */
	const char *plmn = reported;
	bool numeric = reported && strlen(reported) >= 5 && strlen(reported) <= 6;
	for (size_t i = 0; numeric && i < strlen(reported); i++)
		if (reported[i] < '0' || reported[i] > '9')
			numeric = false;
	if (!numeric && reported && *reported) {
		char *raw = controller_at(c, "AT+COPS=3,2", 5);
		free(raw);
		json_object_put(op);
		op = controller_query(c, "AT+COPS?", 50, "+COPS:");
		if (!op)
			return -1;
		n = json_object_array_length(op);
		reported = n > 2 ? json_object_get_string(json_object_array_get_idx(op, 2)) : NULL;
		plmn = reported;
		numeric = reported && strlen(reported) >= 5 && strlen(reported) <= 6;
		for (size_t i = 0; numeric && i < strlen(reported); i++)
			if (reported[i] < '0' || reported[i] > '9')
				numeric = false;
		if (!numeric) {
			/* Still not numeric: recover the digits from the display name
			 * so the brand mapping still resolves. */
			plmn = operator_plmn_from_name(reported);
			if (plmn)
				numeric = true;
		}
	}
	J *operator = json_object_new_object();
	integer(operator, "selection", selection);
	if (numeric && plmn)
		string(operator, "plmn", plmn);
	else
		json_object_object_add(operator, "plmn", NULL);
	string(operator, "name", reported);
	string(operator, "brand", operator_resolve_brand(c, numeric && plmn ? plmn : reported));
	string(operator, "home_brand", operator_home_brand(c));
	if (n > 3)
		integer(operator, "act", act);
	else
		json_object_object_add(operator, "act", NULL);
	json_object_put(op);
	json_object_object_add(c->status, "operator", operator);
	int cfun, attached;
	if (query_int(c, "AT+CFUN?", 50, "+CFUN:", INT_MAX, &cfun) < 0 ||
	    query_int(c, "AT+CGATT?", 50, "+CGATT:", 1, &attached) < 0)
		return -1;
	integer(c->status, "cfun", cfun);
	integer(c->status, "attached", attached);
	if (ims_refresh(c) < 0)
		return -1;
	if (c->data) {
		char *raw = controller_at(c, "AT+CGACT?", 5);
		int active = raw ? bearer_active(raw) : -1;
		free(raw);
		if (active < 0)
			return -1;
		if (!text_equal(str(get(c->status, "sim"), "state"), "READY") || !registered_now ||
		    !active || attached != 1) {
			J *r = controller_disconnect(c, false);
			if (!r)
				return -1;
			json_object_put(r);
		}
	}
	return controller_publish(c);
}

J *controller_execute(struct controller *c, const char *name, J *args)
{
	if (controller_validate(name, args) < 0)
		return NULL;
	const struct operation *op = lookup(name);
	if (op->family == 8)
		return identity_execute(c, name, args);
	if (op->family == 7)
		return operator_execute(c, name, args);
	if (op->family == 6)
		return debug_execute(c, args);
	if (op->family == 1)
		return advanced_execute(c, name, args);
	if (op->family == 2)
		return apn_execute(c, name, args);
	if (op->family == 3)
		return info_execute(c, name);
	if (op->family == 4)
		return mode_execute(c, name, args);
	if (op->family == 5)
		return sms_execute(c, name, args);
	if (!strcmp(name, "apn"))
		return apn_direct(c, args);
	if (!strcmp(name, "policy")) {
		J *changes = json_object_new_object();
		string(changes, "modem.automatic_connect",
		       json_object_get_boolean(get(args, "automatic_connect")) ? "1" : "0");
		string(changes, "modem.roaming",
		       json_object_get_boolean(get(args, "roaming")) ? "1" : "0");
		int rc = cell_config_commit(changes, NULL);
		json_object_put(changes);
		if (rc < 0)
			return NULL;
		J *config = controller_config();
		if (!config)
			return NULL;
		json_object_put(c->config);
		c->config = config;
		c->want_data = json_object_get_boolean(get(args, "automatic_connect"));
		if (!json_object_get_boolean(get(args, "roaming")) && roaming(c)) {
			J *r = controller_disconnect(c, false);
			if (!r)
				return NULL;
			json_object_put(r);
		}
		if (controller_publish(c) < 0)
			return NULL;
		J *out = json_object_new_object();
		boolean(out, "automatic_connect", c->want_data);
		boolean(out, "roaming_allowed", json_object_get_boolean(get(args, "roaming")));
		return out;
	}
	if (!strcmp(name, "status"))
		return jclone(c->status);
	if (!strcmp(name, "refresh")) {
		if (controller_refresh(c) < 0)
			return NULL;
		return jclone(c->status);
	}
	if (!strcmp(name, "connect")) {
		if (json_object_get_boolean(get(args, "automatic")) &&
		    !text_equal(str(c->config, "automatic_connect"), "1")) {
			J *out = json_object_new_object();
			boolean(out, "connected", false);
			boolean(out, "automatic_connect", false);
			return out;
		}
		c->want_data = true;
		return controller_connect(c, 10);
	}
	if (!strcmp(name, "disconnect")) {
		c->want_data = false;
		J *out = controller_disconnect(c, false);
		if (out)
			boolean(out, "connected", false);
		return out;
	}
	if (transport_admit(&c->transport) < 0)
		return NULL;
	if (!strcmp(name, "radio")) {
		bool enabled = json_object_get_boolean(get(args, "enabled"));
		J *changes = json_object_new_object();
		string(changes, "modem.radio_enabled", enabled ? "1" : "0");
		int rc = cell_config_commit(changes, NULL);
		json_object_put(changes);
		if (rc < 0)
			return NULL;
		string(c->config, "radio_enabled", enabled ? "1" : "0");
		c->radio_restore_pending = true;
		c->restore_radio = enabled;
		c->restore_data = text_equal(str(c->config, "automatic_connect"), "1");
		c->want_data = enabled && text_equal(str(c->config, "automatic_connect"), "1");
		if (enabled) {
			/* Apply the saved mode before starting registration. Otherwise the
			 * first connect immediately takes a newly enabled radio offline. */
			if (initialize(c) < 0 || controller_radio_on(c) < 0)
				return NULL;
		} else {
			J *r = controller_disconnect(c, false);
			if (!r)
				return NULL;
			json_object_put(r);
			int state = radio_state(c);
			if (state < 0 || (state == 1 && command(c, "AT+SFUN=5", 50) < 0))
				return NULL;
		}
		int cfun;
		if (query_int(c, "AT+CFUN?", 50, "+CFUN:", INT_MAX, &cfun) < 0)
			return NULL;
		integer(c->status, "cfun", cfun);
		if ((enabled && cfun != 1) || (!enabled && cfun != 0 && cfun != 4)) {
			errorf(CELL_MODEM, "Radio state readback mismatch");
			return NULL;
		}
		c->radio_restore_pending = false;
		if (controller_publish(c) < 0)
			return NULL;
		J *out = json_object_new_object();
		boolean(out, "accepted", true);
		integer(out, "cfun", cfun);
		return out;
	}
	errorf(CELL_INVALID, "Unknown cellular operation");
	return NULL;
}
