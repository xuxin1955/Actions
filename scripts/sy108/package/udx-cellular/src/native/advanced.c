/* SPDX-License-Identifier: GPL-2.0-only */
#include "cellular.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct band {
	const char *name;
	bool fdd;
	uint32_t bit;
};
static const struct band lte[] = {{"B1", true, 1},    {"B3", true, 4},	   {"B5", true, 16},
				  {"B8", true, 128},  {"B34", false, 2},   {"B38", false, 32},
				  {"B39", false, 64}, {"B40", false, 128}, {"B41", false, 256}};
static const struct band nr[] = {{"n1", true, 1},     {"n3", true, 4},	   {"n5", true, 16},
				 {"n8", true, 128},   {"n28", true, 512},  {"n41", false, 16},
				 {"n77", false, 128}, {"n78", false, 256}, {"n79", false, 512}};
static const char *modes[] = {"lte_only", "nr_only", "nr_lte", "nr_lte_wcdma_gsm"},
		  *labels[] = {"LTE", "NR", "NR + LTE", "NR + LTE + WCDMA + GSM"};
static const int mode_codes[] = {3, 128, 131, 134};

static int mode_index(const char *name)
{
	for (int i = 0; i < 4; i++)
		if (text_equal(name, modes[i]))
			return i;
	return -1;
}

static const struct band *find_band(bool is_nr, const char *name)
{
	const struct band *list = is_nr ? nr : lte;
	size_t count = is_nr ? sizeof(nr) / sizeof(*nr) : sizeof(lte) / sizeof(*lte);
	for (size_t i = 0; i < count; i++)
		if (text_equal(name, list[i].name))
			return &list[i];
	return NULL;
}

int advanced_validate(const char *op, J *args)
{
	if (!strcmp(op, "network_preset")) {
		const char *name = str(args, "mode");
		return text_equal(name, "auto") || text_equal(name, "lte_only") ||
			       text_equal(name, "nr_only") || text_equal(name, "nsa_only")
			   ? 0
			   : errorf(CELL_INVALID, "Unknown network preset");
	}
	if (!strcmp(op, "network_configure")) {
		{
			J *nrarg = json_object_new_object();
			string(nrarg, "mode", str(args, "nr_capability"));
			int rc = mode_validate("nr_capability", nrarg);
			json_object_put(nrarg);
			if (rc < 0)
				return -1;
		}
		return mode_index(str(args, "mode")) >= 0
			   ? 0
			   : errorf(CELL_INVALID, "Unknown network mode");
	}
	if (!strcmp(op, "network_mode"))
		return mode_index(str(args, "mode")) >= 0
			   ? 0
			   : errorf(CELL_INVALID, "Unknown network mode");
	if (!strcmp(op, "band_lock")) {
		const char *rats[] = {"lte", "nr"};
		for (size_t r = 0; r < 2; r++) {
			J *a = get(args, rats[r]);
			if (!json_object_is_type(a, json_type_array) ||
			    json_object_array_length(a) > 16)
				return errorf(CELL_INVALID, "Invalid band list");
			for (size_t i = 0; i < json_object_array_length(a); i++) {
				J *v = json_object_array_get_idx(a, i);
				if (!json_object_is_type(v, json_type_string) ||
				    !find_band(r, json_object_get_string(v)))
					return errorf(CELL_INVALID, "Unknown band");
				for (size_t j = 0; j < i; j++)
					if (json_object_equal(v, json_object_array_get_idx(a, j)))
						return errorf(CELL_INVALID, "Repeated band");
			}
		}
		return 0;
	}
	if (!strcmp(op, "cell_lock")) {
		const char *rat = str(args, "rat");
		int64_t arfcn = json_object_get_int64(get(args, "arfcn")),
			pci = json_object_get_int64(get(args, "pci"));
		if ((!text_equal(rat, "lte") && !text_equal(rat, "nr")) ||
		    !json_object_is_type(get(args, "arfcn"), json_type_int) ||
		    !json_object_is_type(get(args, "pci"), json_type_int) || arfcn < 0 ||
		    arfcn > UINT32_MAX || pci < 0 || pci > (text_equal(rat, "nr") ? 1007 : 503))
			return errorf(CELL_INVALID, "Invalid cell parameters");
		return 0;
	}
	if (!strcmp(op, "unlock")) {
		if (text_equal(str(args, "scope"), "cells") &&
		    !text_equal(str(args, "rat"), "lte") && !text_equal(str(args, "rat"), "nr"))
			return errorf(CELL_INVALID, "Select LTE or NR to unlock cells");
		return text_equal(str(args, "scope"), "bands") ||
			       text_equal(str(args, "scope"), "cells")
			   ? 0
			   : errorf(CELL_INVALID, "Unknown unlock scope");
	}
	return !strcmp(op, "advanced_refresh") ? 0
					       : errorf(CELL_INVALID, "Unknown advanced operation");
}

static J *mask_object(uint32_t fdd0, uint32_t fdd1, uint32_t tdd0, uint32_t tdd1)
{
	J *out = json_object_new_object(), *f = json_object_new_array(),
	  *t = json_object_new_array();
	json_object_array_add(f, json_object_new_int64(fdd0));
	json_object_array_add(f, json_object_new_int64(fdd1));
	json_object_array_add(t, json_object_new_int64(tdd0));
	json_object_array_add(t, json_object_new_int64(tdd1));
	json_object_object_add(out, "fdd", f);
	json_object_object_add(out, "tdd", t);
	return out;
}

static J *decode_mask(struct controller *c, bool is_nr, bool capability)
{
	J *row = controller_query(c,
				  capability ? (is_nr ? "AT+SPLBAND=4" : "AT+SPLBAND=5")
					     : (is_nr ? "AT+SPLBAND=3" : "AT+SPLBAND=0"),
				  50, "+SPLBAND:");
	if (!row)
		return NULL;
	size_t count = is_nr ? 4 : 5;
	uint32_t values[5];
	if (json_object_array_length(row) != count)
		goto invalid;
	for (size_t i = 0; i < count; i++) {
		int64_t n;
		if (!integer_text(json_object_get_string(json_object_array_get_idx(row, i)), 0,
				  UINT32_MAX, &n))
			goto invalid;
		values[i] = (uint32_t)n;
	}
	if (!is_nr && values[0])
		goto invalid;
	J *m = is_nr ? mask_object(values[0], values[1], values[2], values[3])
		     : mask_object(values[3], values[4], values[1], values[2]);
	J *out = json_object_new_object(), *bands = json_object_new_array();
	bool any = false;
	for (size_t i = is_nr ? 0 : 1; i < count; i++)
		any |= values[i] != 0;
	const struct band *list = is_nr ? nr : lte;
	size_t n = is_nr ? sizeof(nr) / sizeof(*nr) : sizeof(lte) / sizeof(*lte);
	for (size_t i = 0; i < n; i++) {
		uint32_t mask = (uint32_t)json_object_get_int64(
		    json_object_array_get_idx(get(m, list[i].fdd ? "fdd" : "tdd"), 0));
		if (mask & list[i].bit)
			json_object_array_add(bands, json_object_new_string(list[i].name));
	}
	json_object_object_add(out, "masks", m);
	boolean(out, "unrestricted", !any);
	json_object_object_add(out, "bands", bands);
	json_object_put(row);
	return out;
invalid:
	json_object_put(row);
	errorf(CELL_INVALID, "Invalid complete band mask");
	return NULL;
}

static J *decode(struct controller *c, bool is_nr)
{
	J *selected = decode_mask(c, is_nr, false);
	if (!selected)
		return NULL;
	J *cap = decode_mask(c, is_nr, true);
	if (cap) {
		bool full = !json_object_get_boolean(get(cap, "unrestricted")) &&
			    json_object_equal(get(selected, "masks"), get(cap, "masks"));
		if (full)
			boolean(selected, "unrestricted", true);
		json_object_object_add(selected, "supported_bands",
				       json_object_get(get(cap, "bands")));
		json_object_object_add(selected, "supported_masks",
				       json_object_get(get(cap, "masks")));
		json_object_put(cap);
	} else if (c->transport.poisoned) {
		json_object_put(selected);
		return NULL;
	} else
		string(selected, "capability_error", cell_error);
	return selected;
}

static bool mask_matches(J *actual, J *desired)
{
	if (json_object_equal(get(actual, "masks"), desired))
		return true;
	bool zero = true;
	const char *names[] = {"fdd", "tdd"};
	for (size_t i = 0; i < 2; i++)
		for (size_t j = 0; j < 2; j++)
			zero &= json_object_get_int64(
				    json_object_array_get_idx(get(desired, names[i]), j)) == 0;
	if (zero)
		return json_object_get_boolean(get(actual, "unrestricted"));
	return json_object_get_boolean(get(actual, "unrestricted")) &&
	       json_object_equal(desired, get(actual, "supported_masks"));
}

static J *read_cell_lock(struct controller *c, const char *rat)
{
	int number = text_equal(rat, "nr") ? 16 : 12;
	char wire[64];
	snprintf(wire, sizeof(wire), "AT+SPFORCEFRQ=%d,3", number);
	J *fields = controller_query(c, wire, 10, "+SPFORCEFRQ:");
	if (!fields)
		return NULL;
	size_t n = json_object_array_length(fields);
	int64_t values[4] = {0};
	bool valid = n == 2 || n == 4;
	for (size_t i = 0; valid && i < n; i++)
		valid = integer_text(json_object_get_string(json_object_array_get_idx(fields, i)),
				     0, UINT32_MAX, &values[i]);
	json_object_put(fields);
	if (!valid || values[0] != number || values[1] != 3 ||
	    (n == 4 && values[3] > (number == 16 ? 1007 : 503))) {
		errorf(CELL_INVALID, "Invalid cell-lock readback");
		return NULL;
	}
	J *row = json_object_new_object();
	string(row, "rat", rat);
	boolean(row, "locked", n == 4);
	if (n == 4) {
		integer(row, "arfcn", values[2]);
		integer(row, "pci", values[3]);
	}
	return row;
}

static J *read_bands(struct controller *c)
{
	J *out = json_object_new_object();
	for (int r = 0; r < 2; r++) {
		J *row = decode(c, r);
		if (!row) {
			json_object_put(out);
			return NULL;
		}
		json_object_object_add(out, r ? "nr" : "lte", row);
	}
	return out;
}

static J *workmode(struct controller *c)
{
	J *row = controller_query(c, "AT+SPTESTMODEM?", 5, "+SPTESTMODEM:");
	if (!row)
		return NULL;
	size_t n = json_object_array_length(row);
	if (n != 2 && n != 7)
		goto invalid;
	for (size_t i = 0; i < n; i++)
		if (!integer_text(json_object_get_string(json_object_array_get_idx(row, i)), 0, 255,
				  NULL))
			goto invalid;
	J *out = json_object_new_array();
	for (size_t i = 0; i < 2; i++)
		json_object_array_add(out, json_object_new_int(atoi(json_object_get_string(
					       json_object_array_get_idx(row, i)))));
	json_object_put(row);
	return out;
invalid:
	json_object_put(row);
	errorf(CELL_INVALID, "Incomplete network mode readback");
	return NULL;
}

int advanced_set_mode(struct controller *c, const char *mode, J **result)
{
	*result = NULL;
	int index = mode_index(mode);
	if (index < 0)
		return errorf(CELL_INVALID, "Unknown network mode");
	J *before = workmode(c);
	if (!before)
		return -1;
	J *out = json_object_new_object(), *after = NULL;
	string(out, "mode", mode);
	boolean(out, "readback_verified", true);
	if (json_object_get_int(json_object_array_get_idx(before, 0)) == mode_codes[index]) {
		after = json_object_get(before);
		boolean(out, "unchanged", true);
	} else {
		J *disconnected = controller_disconnect(c, false);
		if (!disconnected)
			goto fail;
		json_object_put(disconnected);
		char command[80];
		int other = json_object_get_int(json_object_array_get_idx(before, 1));
		snprintf(command, sizeof(command), "AT+SPTESTMODE=%d,%d", mode_codes[index], other);
		char *raw = transport_command(&c->transport, command, 120, NULL, true);
		if (!raw)
			goto fail;
		free(raw);
		after = workmode(c);
		if (!after)
			goto fail;
		if (json_object_get_int(json_object_array_get_idx(after, 0)) != mode_codes[index] ||
		    json_object_get_int(json_object_array_get_idx(after, 1)) != other) {
			errorf(CELL_MODEM, "Network mode readback mismatch");
			goto fail;
		}
		json_object_object_add(out, "before", json_object_get(before));
		boolean(out, "async_completion_verified", true);
	}
	json_object_object_add(out, "workmode", json_object_get(after));
	string(c->status, "mode", mode);
	json_object_object_add(c->status, "workmode", json_object_get(after));
	if (controller_publish(c) < 0)
		goto fail;
	json_object_put(after);
	json_object_put(before);
	*result = out;
	return 0;
fail:
	json_object_put(after);
	json_object_put(before);
	json_object_put(out);
	return -1;
}

static J *refresh(struct controller *c)
{
	J *limits = read_bands(c);
	if (!limits)
		return NULL;
	J *value = json_object_new_object();
	json_object_object_add(value, "band_limits", limits);
	J *choices = json_object_new_object();
	for (int r = 0; r < 2; r++) {
		const struct band *list = r ? nr : lte;
		size_t n = r ? sizeof(nr) / sizeof(*nr) : sizeof(lte) / sizeof(*lte);
		J *array = json_object_new_array();
		for (size_t i = 0; i < n; i++)
			json_object_array_add(array, json_object_new_string(list[i].name));
		json_object_object_add(choices, r ? "nr" : "lte", array);
	}
	json_object_object_add(value, "band_choices", choices);
	string(value, "band_choices_evidence", "device reference: udx710-tools advanced.c");
	J *locks = json_object_new_object();
	for (int r = 0; r < 2; r++) {
		const char *rat = r ? "nr" : "lte";
		J *lock = read_cell_lock(c, rat);
		if (!lock) {
			lock = json_object_new_object();
			string(lock, "error", cell_error);
		}
		json_object_object_add(locks, rat, lock);
	}
	json_object_object_add(value, "cell_locks", locks);
	boolean(value, "cell_lock_readback_supported", true);
	boolean(value, "network_mode_write_supported", true);
	J *available = json_object_new_object();
	for (size_t i = 0; i < 4; i++) {
		J *m = json_object_new_object();
		integer(m, "code", mode_codes[i]);
		string(m, "label", labels[i]);
		json_object_object_add(available, modes[i], m);
	}
	json_object_object_add(value, "mode_choices", available);
	J *mode = workmode(c);
	if (!mode) {
		if (cell_error_kind != CELL_MODEM && cell_error_kind != CELL_INVALID) {
			json_object_put(value);
			return NULL;
		}
		string(value, "mode_error", cell_error);
		boolean(value, "network_mode_write_supported", false);
	} else {
		json_object_object_add(value, "workmode", mode);
		int code = json_object_get_int(json_object_array_get_idx(mode, 0));
		const char *name = "custom";
		for (size_t i = 0; i < 4; i++)
			if (code == mode_codes[i])
				name = modes[i];
		string(value, "mode", name);
	}
	merge(c->status, value);
	if (controller_publish(c) < 0) {
		json_object_put(value);
		return NULL;
	}
	return value;
}

static int command(struct controller *c, const char *text, double timeout)
{
	char *raw = controller_at(c, text, timeout);
	if (!raw)
		return -1;
	free(raw);
	return 0;
}

J *advanced_execute(struct controller *c, const char *op, J *args)
{
	if (transport_admit(&c->transport) < 0)
		return NULL;
	if (!strcmp(op, "advanced_refresh"))
		return refresh(c);
	struct radio_change change;
	J *result = NULL, *desired = NULL, *after = NULL;
	if (radio_begin(c, &change) < 0)
		goto end;
	if (!strcmp(op, "network_mode") || !strcmp(op, "network_preset") ||
	    !strcmp(op, "network_configure")) {
		const char *mode = str(args, "mode"), *nrmode = str(args, "nr_capability");
		if (!nrmode)
			nrmode = str(c->config, "nr_capability");
		if (!nrmode)
			nrmode = "sa_nsa";
		if (!strcmp(op, "network_preset")) {
			nrmode = text_equal(mode, "nsa_only") ? "nsa_only" : "sa_nsa";
			mode = text_equal(mode, "lte_only")  ? "lte_only"
			       : text_equal(mode, "nr_only") ? "nr_only"
							     : "nr_lte";
		}
		if (advanced_set_mode(c, mode, &result) < 0)
			goto end;
		J *cap = mode_set_nr(c, nrmode);
		if (!cap) {
			json_object_put(result);
			result = NULL;
			goto end;
		}
		json_object_object_add(result, "nr_capability", cap);
		J *changes = json_object_new_object();
		string(changes, "modem.network_mode", mode);
		string(changes, "modem.nr_capability", nrmode);
		int rc = cell_config_commit(changes, NULL);
		json_object_put(changes);
		if (rc < 0) {
			json_object_put(result);
			result = NULL;
			goto end;
		}
		string(c->config, "network_mode", mode);
		string(c->config, "nr_capability", nrmode);
		goto end;
	}
	if (!strcmp(op, "band_lock") ||
	    (!strcmp(op, "unlock") && text_equal(str(args, "scope"), "bands"))) {
		desired = json_object_new_object();
		for (int r = 0; r < 2; r++) {
			uint32_t fdd = 0, tdd = 0;
			J *bands = get(args, r ? "nr" : "lte");
			if (!strcmp(op, "band_lock"))
				for (size_t i = 0; i < json_object_array_length(bands); i++) {
					const struct band *b =
					    find_band(r, json_object_get_string(
							     json_object_array_get_idx(bands, i)));
					if (b->fdd)
						fdd |= b->bit;
					else
						tdd |= b->bit;
				}
			json_object_object_add(desired, r ? "nr" : "lte",
					       mask_object(fdd, 0, tdd, 0));
			char wire[160];
			if (r)
				snprintf(wire, sizeof(wire), "AT+SPLBAND=2,%u,0,%u,0", fdd, tdd);
			else
				snprintf(wire, sizeof(wire), "AT+SPLBAND=1,0,%u,0,%u,0", tdd, fdd);
			if (command(c, wire, 50) < 0)
				goto end;
		}
		after = read_bands(c);
		if (!after)
			goto end;
		json_object_object_add(c->status, "band_limits", json_object_get(after));
		if (!mask_matches(get(after, "lte"), get(desired, "lte")) ||
		    !mask_matches(get(after, "nr"), get(desired, "nr"))) {
			errorf(CELL_MODEM, "Band readback mismatch");
			goto end;
		}
		result = json_object_new_object();
		json_object_object_add(result, "band_limits", json_object_get(after));
		boolean(result, "readback_verified", true);
	} else {
		const char *rat = str(args, "rat");
		int number = text_equal(rat, "nr") ? 16 : 12;
		char wire[160];
		snprintf(wire, sizeof(wire), "AT+SPFORCEFRQ=%d,0", number);
		if (command(c, wire, 50) < 0)
			goto end;
		bool lock = !strcmp(op, "cell_lock");
		if (lock) {
			snprintf(wire, sizeof(wire), "AT+SPFORCEFRQ=%d,2,%lld,%lld", number,
				 (long long)json_object_get_int64(get(args, "arfcn")),
				 (long long)json_object_get_int64(get(args, "pci")));
			if (command(c, wire, 50) < 0)
				goto end;
		}
		after = read_cell_lock(c, rat);
		if (!after)
			goto end;
		if (json_object_get_boolean(get(after, "locked")) != lock ||
		    (lock && (!json_object_equal(get(after, "arfcn"), get(args, "arfcn")) ||
			      !json_object_equal(get(after, "pci"), get(args, "pci"))))) {
			errorf(CELL_MODEM, "Cell-lock readback mismatch");
			goto end;
		}
		result = json_object_new_object();
		boolean(result, "command_accepted", true);
		boolean(result, "readback_verified", true);
		json_object_object_add(result, "cell_lock", json_object_get(after));
		J *locks = get(c->status, "cell_locks");
		if (!locks) {
			locks = json_object_new_object();
			json_object_object_add(c->status, "cell_locks", locks);
		}
		json_object_object_add(locks, rat, json_object_get(after));
	}
end:
	if (radio_end(c, &change, result != NULL) < 0) {
		json_object_put(result);
		result = NULL;
	}
	json_object_put(desired);
	json_object_put(after);
	return result;
}
