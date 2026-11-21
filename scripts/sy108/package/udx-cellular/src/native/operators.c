/* SPDX-License-Identifier: GPL-2.0-only */
#include "cellular.h"
#include <iconv.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef CELL_PROVIDERS
#define CELL_PROVIDERS "/usr/share/udx-cellular/providers.json"
#endif
static bool plmn_valid(const char *s)
{
	if (!s || (strlen(s) != 5 && strlen(s) != 6))
		return false;
	for (; *s; s++)
		if (!isdigit((unsigned char)*s))
			return false;
	return true;
}

/* PLMN aliases from the pinned AOSP Internet profiles in providers.json. */
const char *operator_brand(const char *plmn)
{
	static const char *const aliases[][2] = {
	    {"46000", "china_mobile"},	{"46002", "china_mobile"},   {"46004", "china_mobile"},
	    {"46007", "china_mobile"},	{"46008", "china_mobile"},   {"46013", "china_mobile"},
	    {"46001", "china_unicom"},	{"46009", "china_unicom"},   {"46003", "china_telecom"},
	    {"46011", "china_telecom"}, {"46015", "china_broadnet"}, {"46060", "china_broadnet"}};
	for (size_t i = 0; i < sizeof(aliases) / sizeof(*aliases); i++)
		if (text_equal(plmn, aliases[i][0]))
			return aliases[i][1];
	return NULL;
}

/* Last-resort brand for a display name that is not a PLMN.  The module keeps
 * the AT+COPS display format in its own non-volatile memory, so any external
 * AT client can switch it to the long-name format and hide the digits.  A
 * Broadnet SIM then reports "China Unicom" or "China Mobile" and the UI shows
 * the wrong carrier.  Match the well known Chinese and English names so the
 * brand stays correct even before the numeric format is restored. */
static const char *brand_by_name(const char *name)
{
	static const char *const names[][2] = {
	    {"中国移动", "china_mobile"}, {"移动", "china_mobile"},   {"China Mobile", "china_mobile"},
	    {"中国联通", "china_unicom"}, {"联通", "china_unicom"},   {"China Unicom", "china_unicom"},
	    {"中国电信", "china_telecom"}, {"电信", "china_telecom"},   {"China Telecom", "china_telecom"},
	    {"中国广电", "china_broadnet"}, {"广电", "china_broadnet"}, {"China Broadnet", "china_broadnet"},
	    {"中国广电 (CBN) NET", "china_broadnet"}};
	if (!name || !*name)
		return NULL;
	for (size_t i = 0; i < sizeof(names) / sizeof(*names); i++)
		if (!strcmp(name, names[i][0]))
			return names[i][1];
	return NULL;
}

/* Brand derived from the SIM's own IMSI prefix.  This is the operator the
 * SIM belongs to and stays correct while the modem roams on a partner
 * network, which is exactly the case where the serving network name is
 * misleading. */
const char *operator_home_brand(struct controller *c)
{
	const char *imsi = str(c->status, "imsi");
	if (!imsi || strlen(imsi) < 5)
		return NULL;
	char plmn[6];
	memcpy(plmn, imsi, 5);
	plmn[5] = 0;
	return operator_brand(plmn);
}

/* Reverse lookup for a display name, so the numeric PLMN survives even when
 * the module is stuck in the long-name display format. */
const char *operator_plmn_from_name(const char *name)
{
	static const char *const names[][2] = {
	    {"中国移动", "46000"}, {"China Mobile", "46000"}, {"移动", "46000"},
	    {"中国联通", "46001"}, {"China Unicom", "46001"}, {"联通", "46001"},
	    {"中国电信", "46003"}, {"China Telecom", "46003"}, {"电信", "46003"},
	    {"中国广电", "46015"}, {"China Broadnet", "46015"}, {"广电", "46015"},
	    {"中国广电 (CBN) NET", "46015"}};
	if (!name || !*name)
		return NULL;
	for (size_t i = 0; i < sizeof(names) / sizeof(*names); i++)
		if (!strcmp(name, names[i][0]))
			return names[i][1];
	return NULL;
}

/* Resolve the brand for a reported operator name.  A numeric PLMN wins; a
 * long display name is matched by name; the SIM's home operator is used when
 * the modem reports something entirely unrecognised. */
const char *operator_resolve_brand(struct controller *c, const char *name)
{
	const char *brand = operator_brand(name);
	if (brand)
		return brand;
	brand = brand_by_name(name);
	if (brand)
		return brand;
	return operator_home_brand(c);
}

int operator_validate(const char *op, J *args)
{
	if (!strcmp(op, "operators_scan") || !strcmp(op, "operator_auto") ||
	    !strcmp(op, "operator_catalog"))
		return 0;
	int64_t act = json_object_get_int64(get(args, "act"));
	return plmn_valid(str(args, "plmn")) &&
		       json_object_is_type(get(args, "act"), json_type_int) && act >= 0 && act <= 13
		   ? 0
		   : errorf(CELL_INVALID, "Invalid PLMN or radio technology");
}

J *operator_catalog(struct controller *c)
{
	J *all = read_json(CELL_PROVIDERS, 262144);
	if (!all)
		return NULL;
	J *rows = get(all, "providers"), *selected = json_object_new_array();
	const char *imsi = str(c->status, "imsi");
	size_t longest = 0;
	if (imsi)
		for (size_t i = 0; i < json_object_array_length(rows); i++) {
			const char *plmn = str(json_object_array_get_idx(rows, i), "plmn");
			if (plmn_valid(plmn) && !strncmp(imsi, plmn, strlen(plmn)) &&
			    strlen(plmn) > longest)
				longest = strlen(plmn);
		}
	for (size_t i = 0; i < json_object_array_length(rows); i++) {
		J *r = json_object_array_get_idx(rows, i);
		const char *plmn = str(r, "plmn");
		if (imsi && plmn_valid(plmn) && strlen(plmn) == longest &&
		    !strncmp(imsi, plmn, longest))
			json_object_array_add(selected, json_object_get(r));
	}
	json_object_object_add(all, "home_candidates", selected);
	if (json_object_array_length(selected))
		string(all, "home_plmn", str(json_object_array_get_idx(selected, 0), "plmn"));
	json_object_object_del(all, "providers");
	return all;
}

/* All catalogue rows for one PLMN, in catalogue order. */
static J *operator_rows_for_plmn(const char *plmn)
{
	if (!plmn_valid(plmn))
		return NULL;
	J *all = read_json(CELL_PROVIDERS, 262144);
	if (!all)
		return NULL;
	J *rows = get(all, "providers"), *out = json_object_new_array();
	for (size_t i = 0; i < json_object_array_length(rows); i++) {
		J *r = json_object_array_get_idx(rows, i);
		if (text_equal(str(r, "plmn"), plmn))
			json_object_array_add(out, json_object_get(r));
	}
	json_object_put(all);
	return out;
}

/* Ordered Internet candidates for automatic mode.
 *
 * The catalogue is keyed by the network the modem is camped on, not by the
 * SIM's owner: a Broadnet SIM (46015, APN cbnet) that roams onto a Unicom
 * cell has to use the Unicom profile (46001, APN 3gnet) or the PDP context
 * never comes up.  A fixed home APN is exactly what made the router show
 * "China Unicom" and then fail to connect.
 *
 * The serving network therefore wins, and the SIM's own operator is only the
 * fallback for the case where the visited network has no catalogue entry. */
J *operator_candidates(struct controller *c)
{
	J *out = json_object_new_array();
	const char *serving = str(get(c->status, "operator"), "plmn");
	char owned[8] = {0};
	if (!serving || !plmn_valid(serving)) {
		/* The cached operator can be stale or absent right after a SIM
		 * change, so read the camped network directly. */
		J *fields = controller_query(c, "AT+COPS?", 50, "+COPS:");
		if (fields) {
			const char *name = json_object_array_length(fields) > 2
						   ? json_object_get_string(
								 json_object_array_get_idx(fields, 2))
						   : NULL;
			if (name && plmn_valid(name))
				snprintf(owned, sizeof(owned), "%s", name);
			else {
				const char *mapped = operator_plmn_from_name(name);
				if (mapped)
					snprintf(owned, sizeof(owned), "%s", mapped);
			}
			json_object_put(fields);
		}
		if (owned[0])
			serving = owned;
	}
	if (serving && plmn_valid(serving)) {
		J *rows = operator_rows_for_plmn(serving);
		for (size_t i = 0; i < json_object_array_length(rows); i++) {
			J *r = json_object_array_get_idx(rows, i);
			if (!get(r, "mvno_type"))
				json_object_array_add(out, json_object_get(r));
		}
		json_object_put(rows);
	}
	if (info_sim_identity(c) < 0)
		return out;
	J *catalog = operator_catalog(c);
	if (!catalog)
		return out;
	J *home = get(catalog, "home_candidates");
	for (size_t i = 0; i < json_object_array_length(home); i++) {
		J *r = json_object_array_get_idx(home, i);
		if (get(r, "mvno_type"))
			continue;
		const char *plmn = str(r, "plmn");
		bool duplicate = false;
		for (size_t j = 0; j < json_object_array_length(out); j++)
			if (text_equal(str(json_object_array_get_idx(out, j), "plmn"), plmn) &&
			    text_equal(str(json_object_array_get_idx(out, j), "apn"),
					str(r, "apn"))) {
				duplicate = true;
				break;
			}
		if (!duplicate)
			json_object_array_add(out, json_object_get(r));
	}
	json_object_put(catalog);
	return out;
}

/* Select the next usable Internet candidate for automatic mode.
 * Returns 1 when a candidate was installed, 0 when none is left.
 *
 * The candidate list is rebuilt on every call, so the cursor cannot be a
 * position into that list: the serving network can appear or disappear while
 * the retry loop runs and the order changes with it.  The identity of the
 * last attempted candidate is remembered instead, and the next call resumes
 * after it. */
int operator_next_candidate(struct controller *c)
{
	if (!text_equal(str(c->config, "apn_mode"), "auto"))
		return 0;
	J *candidates = operator_candidates(c);
	if (!candidates)
		return -1;
	size_t n = json_object_array_length(candidates);
	/* Nothing was attempted yet, so the whole list is still available. */
	bool resumed = c->fallback_tried[0] == 0;
	for (size_t i = 0; i < n; i++) {
		J *r = json_object_array_get_idx(candidates, i);
		char identity[64];
		snprintf(identity, sizeof(identity), "%s/%s", str(r, "plmn"),
			 str(r, "apn"));
		if (!resumed) {
			/* Skip everything up to and including the last attempt. */
			if (text_equal(identity, c->fallback_tried))
				resumed = true;
			continue;
		}
		J *profile = json_object_new_object();
		string(profile, "apn", str(r, "apn"));
		string(profile, "pdp_type", str(r, "pdp_type"));
		integer(profile, "cid", 1);
		int valid = profile_validate(profile);
		json_object_put(profile);
		if (valid < 0 || apn_auth_validate(r) < 0)
			continue;
		c->fallback_index = i + 1;
		snprintf(c->fallback_tried, sizeof(c->fallback_tried), "%s", identity);
		json_object_put(c->fallback_profile);
		c->fallback_profile = jclone(r);
		string(c->status, "automatic_apn", str(r, "apn"));
		string(c->status, "apn_plmn", str(r, "plmn"));
		string(c->status, "home_plmn", str(r, "plmn"));
		json_object_put(candidates);
		return 1;
	}
	json_object_put(candidates);
	return 0;
}

/* A single Internet candidate per SIM, only after a definite activation rejection.
 * Roaming uses the SIM's home identity; manual credentials are never overwritten. */
int operator_fallback(struct controller *c)
{
	if (c->fallback_attempted || !text_equal(str(c->config, "apn_mode"), "auto"))
		return 0;
	c->fallback_attempted = true;
	if (info_sim_identity(c) < 0)
		return -1;
	J *catalog = operator_catalog(c);
	if (!catalog)
		return -1;
	J *rows = get(catalog, "home_candidates");
	for (size_t i = 0; i < json_object_array_length(rows); i++) {
		J *r = json_object_array_get_idx(rows, i);
		if (get(r, "mvno_type"))
			continue;
		J *profile = json_object_new_object();
		string(profile, "apn", str(r, "apn"));
		string(profile, "pdp_type", str(r, "pdp_type"));
		integer(profile, "cid", 1);
		int valid = profile_validate(profile);
		json_object_put(profile);
		if (valid < 0 || apn_auth_validate(r) < 0)
			continue;
		json_object_put(c->fallback_profile);
		c->fallback_profile = jclone(r);
		string(c->status, "automatic_apn", str(r, "apn"));
		string(c->status, "home_plmn", str(r, "plmn"));
		json_object_put(catalog);
		return 1;
	}
	json_object_put(catalog);
	return 0;
}

/* This modem returns raw UCS-2BE operator names even with CSCS="HEX".
 * Keep UTF-8/ASCII names intact and convert that legacy representation before
 * publishing JSON. An undecodable display name must not hide its valid PLMN.
 */
static void operator_name(J *row, const char *key, J *field, const char *plmn)
{
	const char *raw = json_object_get_string(field);
	size_t length = json_object_get_string_len(field);
	const char *encodings[] = {"UTF-8", "UTF-16BE"};
	char *text = malloc(length * 3 + 1);
	if (!text) {
		string(row, key, plmn);
		return;
	}
	for (size_t i = 0; i < 2; i++) {
		iconv_t cd = iconv_open("UTF-8", encodings[i]);
		if (cd == (iconv_t)-1)
			continue;
		char *input = (char *)raw, *output = text;
		size_t left = length, space = length * 3;
		size_t result = iconv(cd, &input, &left, &output, &space);
		iconv_close(cd);
		if (result != (size_t)-1 && !left) {
			*output = 0;
			string(row, key, text);
			free(text);
			return;
		}
	}
	free(text);
	string(row, key, plmn);
}

J *operator_parse_scan(const char *raw)
{
	if (!at_complete(raw)) {
		errorf(CELL_INVALID, "Incomplete operator scan");
		return NULL;
	}
	const char *p = strstr(raw, "+COPS:");
	if (!p) {
		errorf(CELL_INVALID, "Missing operator scan");
		return NULL;
	}
	p += 6;
	J *rows = json_object_new_array();
	while (*p && *p != '\r' && *p != '\n') {
		while (*p == ' ' || *p == ',')
			p++;
		if (*p != '(')
			break;
		const char *start = ++p;
		bool quote = false;
		for (; *p && *p != '\r' && *p != '\n'; p++) {
			if (*p == '"')
				quote = !quote;
			else if (*p == ')' && !quote)
				break;
		}
		if (*p != ')' || p - start > 1024)
			goto bad;
		char wire[1100];
		snprintf(wire, sizeof(wire), "+ROW:%.*s\r\nOK\r\n", (int)(p - start), start);
		p++;
		J *fields = at_one(wire, "+ROW:");
		if (!fields)
			goto bad;
		size_t n = json_object_array_length(fields);
		if (memchr(start, '"', (size_t)(p - start - 1))) {
			const char *plmn =
			    json_object_get_string(json_object_array_get_idx(fields, 3));
			int64_t status, act = 0;
			if (json_object_array_length(rows) >= 64 || (n != 4 && n != 5) ||
			    !plmn_valid(plmn) ||
			    !integer_text(
				json_object_get_string(json_object_array_get_idx(fields, 0)), 0, 3,
				&status) ||
			    (n == 5 && !integer_text(json_object_get_string(
							 json_object_array_get_idx(fields, 4)),
						     0, 13, &act))) {
				json_object_put(fields);
				goto bad;
			}
			J *r = json_object_new_object();
			integer(r, "status", status);
			integer(r, "act", act);
			string(r, "plmn", plmn);
			operator_name(r, "name", json_object_array_get_idx(fields, 1), plmn);
			operator_name(r, "short_name", json_object_array_get_idx(fields, 2), plmn);
			json_object_array_add(rows, r);
		}
		json_object_put(fields);
	}
	return rows;
bad:
	json_object_put(rows);
	errorf(CELL_INVALID, "Malformed operator scan");
	return NULL;
}

J *operator_execute(struct controller *c, const char *op, J *args)
{
	if (!strcmp(op, "operator_catalog"))
		return operator_catalog(c);
	if (!radio_enabled(c)) {
		errorf(CELL_INVALID, "Enable radio before operator selection");
		return NULL;
	}
	if (!strcmp(op, "operators_scan")) {
		char *raw = controller_at(c, "AT+COPS=?", 180);
		J *rows = raw ? operator_parse_scan(raw) : NULL;
		free(raw);
		if (!rows)
			return NULL;
		J *out = json_object_new_object();
		json_object_object_add(out, "operators", rows);
		return out;
	}
	bool wanted = c->want_data;
	c->want_data = false;
	J *disconnected = controller_disconnect(c, false);
	if (!disconnected) {
		c->want_data = wanted;
		return NULL;
	}
	json_object_put(disconnected);
	J *out = NULL, *fields = NULL;
	char *raw = NULL;
	char wire[80];
	/* Configuration permits RF; verify/enable the actual modem before COPS. */
	if (controller_radio_on(c) < 0)
		goto done;
	if (!strcmp(op, "operator_auto"))
		snprintf(wire, sizeof(wire), "AT+COPS=0");
	else
		snprintf(wire, sizeof(wire), "AT+COPS=1,2,\"%s\",%d", str(args, "plmn"),
			 json_object_get_int(get(args, "act")));
	raw = controller_at(c, wire, 180);
	if (!raw)
		goto done;
	free(raw);
	raw = controller_at(c, "AT+COPS=3,2", 5);
	if (!raw)
		goto done;
	fields = controller_query(c, "AT+COPS?", 10, "+COPS:");
	if (!fields)
		goto done;
	bool automatic = !strcmp(op, "operator_auto");
	bool verified =
	    text_equal(json_object_get_string(json_object_array_get_idx(fields, 0)),
		       automatic ? "0" : "1") &&
	    (automatic ||
	     (text_equal(json_object_get_string(json_object_array_get_idx(fields, 2)),
			 str(args, "plmn")) &&
	      text_equal(json_object_get_string(json_object_array_get_idx(fields, 1)), "2")));
	if (!verified) {
		errorf(CELL_MODEM, "Operator selection readback mismatch");
		goto done;
	}
	out = json_object_new_object();
	boolean(out, "readback_verified", true);
done:
	free(raw);
	json_object_put(fields);
	c->want_data = wanted;
	return out;
}
