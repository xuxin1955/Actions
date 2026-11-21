/* SPDX-License-Identifier: GPL-2.0-only */
#include "cellular.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool digits(const char *s)
{
	if (!s || strlen(s) != 15)
		return false;
	for (size_t i = 0; i < 15; i++)
		if (!isdigit((unsigned char)s[i]))
			return false;
	return true;
}

int identity_validate(const char *operation, J *args)
{
	if (!strcmp(operation, "imei_read"))
		return 0;
	int slot = json_object_get_int(get(args, "slot"));
	const char *imei = str(args, "imei");
	if (!json_object_is_type(get(args, "slot"), json_type_int) || slot < 0 || slot > 1 ||
	    !digits(imei))
		return errorf(CELL_INVALID, "IMEI requires 15 digits and logical identity 0 or 1");
	unsigned sum = 0;
	for (unsigned i = 0; i < 15; i++) {
		unsigned n = imei[i] - '0';
		if (i % 2)
			n *= 2;
		sum += n / 10 + n % 10;
	}
	return sum % 10 ? errorf(CELL_INVALID, "IMEI Luhn check failed") : 0;
}

J *identity_parse(const char *raw)
{
	if (!at_complete(raw)) {
		errorf(CELL_INVALID, "Incomplete IMEI response");
		return NULL;
	}
	J *rows = json_object_new_array();
	char *plain = info_identifier(raw, "AT+SPIMEI?", "+SPIMEI:", 15, 15, false);
	if (plain) {
		J *r = json_object_new_object();
		integer(r, "slot", 0);
		string(r, "imei", plain);
		free(plain);
		json_object_array_add(rows, r);
		return rows;
	}
	J *fields = at_fields(raw, "+SPIMEI:");
	if (!fields)
		goto bad;
	for (size_t i = 0; i < json_object_array_length(fields); i++) {
		J *f = json_object_array_get_idx(fields, i);
		int64_t slot;
		const char *imei = json_object_get_string(json_object_array_get_idx(f, 1));
		if (json_object_array_length(f) != 2 ||
		    !integer_text(json_object_get_string(json_object_array_get_idx(f, 0)), 0, 1,
				  &slot) ||
		    !digits(imei)) {
			json_object_put(fields);
			goto bad;
		}
		for (size_t j = 0; j < json_object_array_length(rows); j++)
			if (json_object_get_int(get(json_object_array_get_idx(rows, j), "slot")) ==
			    slot) {
				json_object_put(fields);
				goto bad;
			}
		J *r = json_object_new_object();
		integer(r, "slot", slot);
		string(r, "imei", imei);
		json_object_array_add(rows, r);
	}
	json_object_put(fields);
	if (json_object_array_length(rows))
		return rows;
bad:
	json_object_put(rows);
	errorf(CELL_INVALID, "Missing or ambiguous IMEI identities");
	return NULL;
}

static J *read_identities(struct controller *c)
{
	char *raw = controller_at(c, "AT+SPIMEI?", 5);
	J *rows = raw ? identity_parse(raw) : NULL;
	free(raw);
	return rows;
}

static const char *lookup(J *rows, int slot)
{
	for (size_t i = 0; i < json_object_array_length(rows); i++) {
		J *r = json_object_array_get_idx(rows, i);
		if (json_object_get_int(get(r, "slot")) == slot)
			return str(r, "imei");
	}
	return NULL;
}

J *identity_execute(struct controller *c, const char *operation, J *args)
{
	J *rows = read_identities(c);
	if (!rows)
		return NULL;
	if (!strcmp(operation, "imei_read")) {
		J *out = json_object_new_object();
		json_object_object_add(out, "identities", rows);
		return out;
	}
	int slot = json_object_get_int(get(args, "slot"));
	if (!lookup(rows, slot)) {
		json_object_put(rows);
		errorf(CELL_INVALID,
		       "This modem does not expose readback for the selected IMEI identity");
		return NULL;
	}
	json_object_put(rows);
	rows = NULL;
	struct radio_change change;
	bool ok = radio_begin(c, &change) == 0;
	if (ok) {
		char wire[80];
		snprintf(wire, sizeof(wire), "AT+SPIMEI=%d,\"%s\"", slot, str(args, "imei"));
		ok = radio_command(c, wire, 10) == 0;
	}
	if (ok) {
		rows = read_identities(c);
		ok = rows && text_equal(lookup(rows, slot), str(args, "imei"));
		if (!ok)
			errorf(CELL_MODEM, "IMEI readback mismatch");
	}
	if (radio_end(c, &change, ok) < 0) {
		json_object_put(rows);
		return NULL;
	}
	J *out = json_object_new_object();
	boolean(out, "readback_verified", true);
	json_object_object_add(out, "identities", rows);
	return out;
}
