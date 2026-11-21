/* SPDX-License-Identifier: GPL-2.0-only */
#include "cellular.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *trim(char *s)
{
	while (isspace((unsigned char)*s))
		s++;
	size_t n = strlen(s);
	while (n && isspace((unsigned char)s[n - 1]))
		s[--n] = 0;
	return s;
}

static J *useful(const char *raw, const char *command)
{
	if (!at_complete(raw)) {
		errorf(CELL_INVALID, "Incomplete information response");
		return NULL;
	}
	J *rows = json_object_new_array();
	char *copy = strdup(raw), *save = NULL;
	if (!copy) {
		json_object_put(rows);
		errorf(CELL_SYSTEM, "Information response allocation failed");
		return NULL;
	}
	for (char *line = strtok_r(copy, "\r\n", &save); line;
	     line = strtok_r(NULL, "\r\n", &save)) {
		line = trim(line);
		if (!*line || !strcmp(line, "OK") || text_equal(line, command) ||
		    !strncmp(line, "+CEREG:", 7) || !strncmp(line, "+C5GREG:", 8) ||
		    !strncmp(line, "+CGREG:", 7) || !strncmp(line, "+CGEV:", 6) ||
		    !strncmp(line, "+CMTI:", 6))
			continue;
		json_object_array_add(rows, json_object_new_string(line));
	}
	free(copy);
	return rows;
}

char *info_identifier(const char *raw, const char *command, const char *prefix, size_t min,
		      size_t max, bool hex)
{
	J *rows = useful(raw, command);
	if (!rows)
		return NULL;
	char *result = NULL;
	unsigned count = 0;
	for (size_t i = 0; i < json_object_array_length(rows); i++) {
		const char *line = json_object_get_string(json_object_array_get_idx(rows, i));
		if (*prefix && !strncmp(line, prefix, strlen(prefix)))
			line += strlen(prefix);
		while (isspace((unsigned char)*line) || *line == '"')
			line++;
		size_t n = strlen(line);
		while (n && (isspace((unsigned char)line[n - 1]) || line[n - 1] == '"'))
			n--;
		bool valid = n >= min && n <= max;
		for (size_t j = 0; valid && j < n; j++)
			if (hex ? !isxdigit((unsigned char)line[j])
				: !isdigit((unsigned char)line[j]))
				valid = false;
		if (valid) {
			count++;
			free(result);
			result = strndup(line, n);
		}
	}
	json_object_put(rows);
	if (count != 1) {
		free(result);
		errorf(CELL_INVALID, "Missing or ambiguous identifier");
		return NULL;
	}
	return result;
}

static J *matrix(const char *raw)
{
	if (!at_complete(raw)) {
		errorf(CELL_INVALID, "Incomplete SPENGMD response");
		return NULL;
	}
	char *copy = strdup(raw), *save = NULL, *selected = NULL;
	unsigned count = 0;
	if (!copy) {
		errorf(CELL_SYSTEM, "Cell response allocation failed");
		return NULL;
	}
	for (char *line = strtok_r(copy, "\r\n", &save); line;
	     line = strtok_r(NULL, "\r\n", &save)) {
		line = trim(line);
		if (isdigit((unsigned char)*line) ||
		    (*line == '-' && isdigit((unsigned char)line[1]))) {
			selected = line;
			count++;
		}
	}
	if (count != 1 || strlen(selected) > 32768) {
		free(copy);
		errorf(CELL_INVALID, "Missing or ambiguous SPENGMD matrix");
		return NULL;
	}
	J *rows = json_object_new_array(), *row = json_object_new_array();
	json_object_array_add(rows, row);
	char *p = selected;
	while (*p) {
		while (*p == ' ' || *p == '\t')
			p++;
		if (!isdigit((unsigned char)*p) && !(*p == '-' && isdigit((unsigned char)p[1])))
			goto invalid;
		errno = 0;
		char *end;
		long long value = strtoll(p, &end, 10);
		if (errno || end == p)
			goto invalid;
		p = end;
		while (*p == ' ' || *p == '\t')
			p++;
		json_object_array_add(row, json_object_new_int64(value));
		if (json_object_array_length(rows) > 64 || json_object_array_length(row) > 16)
			goto invalid;
		if (!*p) {
			free(copy);
			return rows;
		}
		char separator = *p++;
		if (separator == '-') {
			row = json_object_new_array();
			json_object_array_add(rows, row);
		} else if (separator != ',')
			goto invalid;
	}
invalid:
	free(copy);
	json_object_put(rows);
	errorf(CELL_INVALID, "Malformed, overflowing or incomplete SPENGMD matrix");
	return NULL;
}

static int infer_band(bool nr, int64_t arfcn)
{
	static const int64_t lte[][3] = {{0, 599, 1},	     {1200, 1949, 3},	 {2400, 2649, 5},
					 {2750, 3449, 7},    {3450, 3799, 8},	 {6150, 6449, 20},
					 {9210, 9659, 28},   {37750, 38249, 38}, {38250, 38649, 39},
					 {38650, 39649, 40}, {39650, 41589, 41}};
	static const int64_t nr_ranges[][3] = {
	    {422000, 434000, 1},  {361000, 376000, 3},	{173800, 178800, 5}, {185000, 192000, 8},
	    {151600, 160600, 28}, {499200, 537999, 41}, {693334, 733333, 79}};
	const int64_t (*ranges)[3] = nr ? nr_ranges : lte;
	size_t n = nr ? sizeof(nr_ranges) / sizeof(*nr_ranges) : sizeof(lte) / sizeof(*lte);
	for (size_t i = 0; i < n; i++)
		if (arfcn >= ranges[i][0] && arfcn <= ranges[i][1])
			return (int)ranges[i][2];
	return 0;
}

static int64_t element(J *rows, size_t r, size_t c)
{
	return json_object_get_int64(
	    json_object_array_get_idx(json_object_array_get_idx(rows, r), c));
}

J *decode_cells(const char *raw, const char *rat, bool serving)
{
	J *rows = matrix(raw);
	if (!rows)
		return NULL;
	J *out = json_object_new_array();
	bool any = false, nr = text_equal(rat, "nr");
	size_t n = json_object_array_length(rows);
	for (size_t r = 0; r < n; r++)
		for (size_t c = 0; c < json_object_array_length(json_object_array_get_idx(rows, r));
		     c++)
			any |= element(rows, r, c) != 0;
	if (!any) {
		json_object_put(rows);
		return out;
	}
	size_t count = serving ? 1
		       : nr    ? json_object_array_length(json_object_array_get_idx(rows, 0))
			       : n;
	if (serving && n <= (nr ? 15 : 33))
		goto invalid;
	if (!serving && nr) {
		if (n < 6)
			goto invalid;
		for (size_t r = 0; r < 6; r++)
			if (json_object_array_length(json_object_array_get_idx(rows, r)) != count)
				goto invalid;
	}
	for (size_t i = 0; i < count; i++) {
		int64_t v[6];
		if (serving) {
			for (size_t r = 0; r < 5; r++)
				v[r] = element(rows, r, 0);
			v[5] = element(rows, nr ? 15 : 33, 0);
		} else if (nr) {
			for (size_t r = 0; r < 6; r++)
				v[r] = element(rows, r, i);
		} else {
			size_t columns =
			    json_object_array_length(json_object_array_get_idx(rows, i));
			if (columns < 7)
				goto invalid;
			v[0] = columns > 12 ? element(rows, i, 12) : 0;
			v[1] = element(rows, i, 0);
			v[2] = element(rows, i, 1);
			v[3] = element(rows, i, 2);
			v[4] = element(rows, i, 3);
			v[5] = element(rows, i, 6);
		}
		bool present = false;
		for (size_t k = 0; k < 6; k++)
			present |= v[k] != 0;
		if (!present)
			continue;
		if (v[0] < 0 || v[0] > 1024 || v[1] < 0 || v[1] > UINT32_MAX || v[2] < 0 ||
		    v[2] > (nr ? 1007 : 503))
			goto invalid;
		int band = v[0] ? (int)v[0] : infer_band(nr, v[1]);
		if (!band && nr && v[1] > 653333 && v[1] <= 680000)
			band = 77;
		J *row = json_object_new_object();
		string(row, "rat", rat);
		boolean(row, "serving", serving);
		char label[32];
		snprintf(label, sizeof(label), "%s%d", nr ? "n" : "B", band);
		string(row, "band", band ? label : NULL);
		string(row, "band_source",
		       v[0]   ? "modem"
		       : band ? "arfcn-inferred"
			      : "unavailable");
		J *candidates = json_object_new_array();
		if (!band && nr && v[1] >= 620000 && v[1] <= 680000) {
			json_object_array_add(candidates, json_object_new_string("n77"));
			if (v[1] <= 653333)
				json_object_array_add(candidates, json_object_new_string("n78"));
		}
		json_object_object_add(row, "band_candidates", candidates);
		integer(row, "arfcn", v[1]);
		integer(row, "pci", v[2]);
		const char *names[] = {"rsrp", "rsrq", "sinr"};
		int lows[] = {-16000, -5000, -5000}, highs[] = {-2000, 1000, 5000};
		for (size_t k = 0; k < 3; k++) {
			if (v[k + 3] >= lows[k] && v[k + 3] <= highs[k])
				real(row, names[k], v[k + 3] / 100.0);
			else
				json_object_object_add(row, names[k], NULL);
		}
		json_object_array_add(out, row);
	}
	json_object_put(rows);
	return out;
invalid:
	json_object_put(rows);
	json_object_put(out);
	errorf(CELL_INVALID, "Incomplete cell matrix or invalid cell identity");
	return NULL;
}

static bool recoverable(void)
{
	return cell_error_kind == CELL_INVALID || cell_error_kind == CELL_MODEM;
}

int info_sim_identity(struct controller *c)
{
	J *errors = get(c->status, "info_errors") ? jclone(get(c->status, "info_errors"))
						  : json_object_new_object();
	const char *keys[] = {"iccid", "imsi"}, *commands[] = {"AT+CCID", "AT+CIMI"},
		   *prefixes[] = {"+CCID:", ""};
	for (size_t i = 0; i < 2; i++) {
		char *raw = controller_at(c, commands[i], 8);
		char *value = raw ? info_identifier(raw, commands[i], prefixes[i], i ? 14 : 19,
						    i ? 16 : 22, !i)
				  : NULL;
		free(raw);
		if (!value) {
			if (!recoverable()) {
				json_object_put(errors);
				return -1;
			}
			string(errors, keys[i], cell_error);
		} else
			json_object_object_del(errors, keys[i]);
		string(c->status, keys[i], value);
		free(value);
	}
	json_object_object_add(c->status, "info_errors", errors);
	return 0;
}

J *info_signal(struct controller *c)
{
	J *f = controller_query(c, "AT+CSQ", 5, "+CSQ:");
	int64_t rssi = 0, ber = 0;
	/*
	 * The UDX710 module does not use the 3GPP TS 27.007 CSQ scale
	 * (rssi 0..31 / ber 0..7).  Both fields are vendor magnitudes in
	 * the 0..99 range, e.g. "+CSQ: 43,16" while the serving cell
	 * reports RSRP around -97 dBm.  Rejecting anything above 31 made
	 * every signal_refresh fail with "Invalid CSQ range" and left the
	 * UI stuck on the stale pre-registration 99,99 snapshot.
	 *
	 * Pairing AT+CSQ with AT+SPENGMD over 15+ samples on both LTE and
	 * NR gives rssi_dbm = csq - 140 with ~1.3 dB mean error.
	 */
	bool valid =
	    f && json_object_array_length(f) == 2 &&
	    integer_text(json_object_get_string(json_object_array_get_idx(f, 0)), 0, 99, &rssi) &&
	    integer_text(json_object_get_string(json_object_array_get_idx(f, 1)), 0, 99, &ber);
	if (f && !valid)
		errorf(CELL_INVALID, "Invalid CSQ range");
	json_object_put(f);
	if (!valid)
		return NULL;
	J *out = json_object_new_object();
	integer(out, "csq", rssi);
	integer(out, "ber", ber);
	string(out, "rssi_scale", "vendor");
	if (rssi != 99)
		integer(out, "rssi_dbm", rssi - 140);
	else
		json_object_object_add(out, "rssi_dbm", NULL);
	string(out, "rssi_limit", rssi == 99 ? "unknown" : NULL);
	real(out, "uptime", mono());
	json_object_object_add(c->status, "signal", json_object_get(out));
	return out;
}

J *info_execute(struct controller *c, const char *operation)
{
	if (transport_admit(&c->transport) < 0)
		return NULL;
	if (!strcmp(operation, "qos_refresh")) {
		char *raw = controller_at(c, "AT+CGEQOSRDP", 5);
		J *rows = raw ? at_fields(raw, "+CGEQOSRDP:") : NULL;
		free(raw);
		if (!rows)
			return NULL;
		J *selected = NULL;
		for (size_t i = 0; i < json_object_array_length(rows); i++) {
			J *row = json_object_array_get_idx(rows, i);
			if (text_equal(json_object_get_string(json_object_array_get_idx(row, 0)),
				       "1")) {
				if (selected) {
					selected = NULL;
					break;
				}
				selected = row;
			}
		}
		int64_t qci;
		if (!selected || json_object_array_length(selected) < 2 ||
		    !integer_text(json_object_get_string(json_object_array_get_idx(selected, 1)), 0,
				  255, &qci)) {
			json_object_put(rows);
			errorf(CELL_INVALID, "No valid CID 1 negotiated QoS response");
			return NULL;
		}
		J *qos = json_object_new_object();
		integer(qos, "cid", 1);
		integer(qos, "qci", qci);
		const char *rates[] = {"gbr_downlink_kbps",  "gbr_uplink_kbps",
				       "mbr_downlink_kbps",  "mbr_uplink_kbps",
				       "ambr_downlink_kbps", "ambr_uplink_kbps"};
		for (size_t i = 0; i < 6; i++) {
			const char *field =
			    json_object_get_string(json_object_array_get_idx(selected, i + 2));
			int64_t rate;
			if (field && *field) {
				if (!integer_text(field, 0, UINT32_MAX, &rate)) {
					json_object_put(qos);
					json_object_put(rows);
					errorf(CELL_INVALID, "Invalid negotiated QoS rate");
					return NULL;
				}
				integer(qos, rates[i], rate);
			} else
				json_object_object_add(qos, rates[i], NULL);
		}
		json_object_object_add(qos, "downlink_kbps",
				       json_object_get(get(qos, "ambr_downlink_kbps")));
		json_object_object_add(qos, "uplink_kbps",
				       json_object_get(get(qos, "ambr_uplink_kbps")));
		real(qos, "uptime", mono());
		json_object_object_add(c->status, "qos", json_object_get(qos));
		json_object_put(rows);
		if (controller_publish(c) < 0) {
			json_object_put(qos);
			return NULL;
		}
		return qos;
	}
	if (!strcmp(operation, "signal_refresh")) {
		J *signal = info_signal(c);
		if (signal && controller_publish(c) < 0) {
			json_object_put(signal);
			return NULL;
		}
		return signal;
	}
	J *errors = json_object_new_object(), *out = NULL;
	bool all_cells = !strcmp(operation, "cells_refresh");
	bool serving_only = !strcmp(operation, "serving_refresh");
	if (all_cells || serving_only || !strcmp(operation, "neighbors_refresh")) {
		const char *commands[] = {"AT+SPENGMD=0,6,0", "AT+SPENGMD=0,6,6",
					  "AT+SPENGMD=0,14,1", "AT+SPENGMD=0,14,2"},
			   *names[] = {"lte_serving", "lte_neighbors", "nr_serving",
				       "nr_neighbors"};
		J *cells = get(c->status, "cells") ? jclone(get(c->status, "cells"))
						   : json_object_new_array();
		json_object_put(errors);
		errors = get(c->status, "cell_query_errors")
			     ? jclone(get(c->status, "cell_query_errors"))
			     : json_object_new_object();
		for (size_t i = 0; i < 4; i++) {
			if (!all_cells && ((i % 2 == 0) != serving_only))
				continue;
			char *raw = controller_at(c, commands[i], 15);
			J *rows = raw ? decode_cells(raw, i < 2 ? "lte" : "nr", !(i % 2)) : NULL;
			free(raw);
			if (!rows) {
				if (!recoverable()) {
					json_object_put(cells);
					goto fail;
				}
				string(errors, names[i], cell_error);
			} else {
				json_object_object_del(errors, names[i]);
				for (size_t j = json_object_array_length(cells); j > 0; j--) {
					J *old = json_object_array_get_idx(cells, j - 1);
					if (text_equal(str(old, "rat"), i < 2 ? "lte" : "nr") &&
					    json_object_get_boolean(get(old, "serving")) ==
						!(i % 2))
						json_object_array_del_idx(cells, j - 1, 1);
				}
				for (size_t j = 0; j < json_object_array_length(rows); j++) {
					J *sample = json_object_array_get_idx(rows, j);
					if (!(i % 2)) {
						J *reg = get(c->status, i < 2 ? "registration"
									      : "nr_registration");
						string(sample, "tac", str(reg, "tac"));
						string(sample, "cell_id", str(reg, "cell_id"));
						json_object_object_add(sample, "bandwidth_mhz",
								       NULL);
					}
					real(sample, "sample_uptime", mono());
					json_object_array_add(cells, json_object_get(sample));
				}
				json_object_put(rows);
			}
		}
		json_object_object_add(c->status, "cells", json_object_get(cells));
		real(c->status, "cells_uptime", mono());
		json_object_object_add(c->status, "cell_query_errors", json_object_get(errors));
		out = json_object_new_object();
		json_object_object_add(out, "cells", cells);
		json_object_object_add(out, "cell_query_errors", json_object_get(errors));
		json_object_object_add(out, "cells_uptime",
				       json_object_get(get(c->status, "cells_uptime")));
		if (controller_publish(c) < 0)
			goto fail;
		json_object_put(errors);
		return out;
	}
	if (controller_refresh(c) < 0) {
		if (!recoverable())
			goto fail;
		string(errors, "registration", cell_error);
	}
	J *values = json_object_new_object();
	const char *names[] = {"imei", "iccid", "imsi"},
		   *commands[] = {"AT+SPIMEI?", "AT+CCID", "AT+CIMI"},
		   *prefixes[] = {"+SPIMEI:", "+CCID:", ""};
	size_t mins[] = {15, 19, 14}, maxs[] = {15, 22, 16};
	for (size_t i = 0; i < 3; i++) {
		char *raw = controller_at(c, commands[i], 8);
		char *value =
		    raw ? info_identifier(raw, commands[i], prefixes[i], mins[i], maxs[i], i == 1)
			: NULL;
		free(raw);
		if (!value) {
			if (!recoverable()) {
				json_object_put(values);
				goto fail;
			}
			string(errors, names[i], cell_error);
		}
		if (value)
			string(values, names[i], value);
		free(value);
	}
	const char *text_names[] = {"manufacturer", "model", "revision"},
		   *text_commands[] = {"AT+CGMI", "AT+CGMM", "AT+CGMR"};
	for (size_t i = 0; i < 3; i++) {
		char *raw = controller_at(c, text_commands[i], 8);
		J *lines = raw ? useful(raw, text_commands[i]) : NULL;
		free(raw);
		char text[2048] = "";
		size_t used = 0, total = 0;
		bool valid = lines && json_object_array_length(lines) > 0;
		if (lines)
			for (size_t j = 0; j < json_object_array_length(lines); j++) {
				const char *s =
				    json_object_get_string(json_object_array_get_idx(lines, j));
				size_t len = strlen(s);
				total += len;
				if (total > 1024 || used + len + 2 >= sizeof(text)) {
					valid = false;
					break;
				}
				if (j)
					text[used++] = '\n';
				memcpy(text + used, s, len + 1);
				used += len;
			}
		if (lines && !valid)
			errorf(CELL_INVALID, json_object_array_length(lines)
						 ? "Invalid modem text response"
						 : "Modem returned OK without information");
		json_object_put(lines);
		if (!valid) {
			if (!recoverable()) {
				json_object_put(values);
				goto fail;
			}
			string(errors, text_names[i], cell_error);
		}
		if (valid)
			string(values, text_names[i], text);
	}
	J *signal = info_signal(c);
	if (!signal) {
		if (!recoverable()) {
			json_object_put(values);
			goto fail;
		}
		string(errors, "signal", cell_error);
	} else {
		json_object_object_add(values, "signal", signal);
	}
	merge(c->status, values);
	real(c->status, "modem_info_uptime", mono());
	json_object_object_add(c->status, "info_errors", json_object_get(errors));
	json_object_put(values);
	if (controller_publish(c) < 0)
		goto fail;
	out = json_object_new_object();
	json_object_object_add(out, "modem_info_uptime",
			       json_object_get(get(c->status, "modem_info_uptime")));
	json_object_object_add(out, "info_errors", json_object_get(errors));
	J *fields = json_object_new_array();
	const char *all[] = {"iccid", "imei",	  "imsi",  "manufacturer",
			     "model", "revision", "signal"};
	for (size_t i = 0; i < 7; i++)
		json_object_array_add(fields, json_object_new_string(all[i]));
	json_object_object_add(out, "updated_fields", fields);
	json_object_put(errors);
	return out;
fail:
	json_object_put(errors);
	json_object_put(out);
	return NULL;
}
