/* SPDX-License-Identifier: GPL-2.0-only */
/* Modem CSV parsing. */
#include "common.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

bool at_complete(const char *raw)
{
	if (!raw)
		return false;
	size_t n = strlen(raw);
	while (n && isspace((unsigned char)raw[n - 1]))
		n--;
	return n >= 2 && raw[n - 2] == 'O' && raw[n - 1] == 'K' &&
	       (n == 2 || raw[n - 3] == '\r' || raw[n - 3] == '\n');
}

static J *csv(const char *p, size_t n)
{
	J *row = json_object_new_array();
	char *buffer = malloc(n + 1);
	if (!buffer) {
		json_object_put(row);
		return NULL;
	}
	if (!n) {
		free(buffer);
		return row;
	}
	size_t i = 0;
	for (;;) {
		size_t used = 0;
		bool quoted = i < n && p[i] == '"';
		if (quoted)
			i++;
		while (i < n) {
			char c = p[i++];
			if (quoted && c == '"') {
				if (i < n && p[i] == '"') {
					buffer[used++] = '"';
					i++;
					continue;
				}
				quoted = false;
				if (i < n && p[i] != ',')
					goto bad;
				break;
			}
			if (!quoted && c == ',') {
				i--;
				break;
			}
			buffer[used++] = c;
		}
		if (quoted || json_object_array_length(row) >= 1024)
			goto bad;
		buffer[used] = 0;
		json_object_array_add(row, json_object_new_string_len(buffer, (int)used));
		if (i == n)
			break;
		if (p[i++] != ',')
			goto bad;
		if (i == n) {
			json_object_array_add(row, json_object_new_string(""));
			break;
		}
	}
	free(buffer);
	return row;
bad:
	free(buffer);
	json_object_put(row);
	errorf(CELL_INVALID, "Malformed AT CSV row");
	return NULL;
}

J *at_fields(const char *raw, const char *prefix)
{
	if (!at_complete(raw)) {
		errorf(CELL_INVALID, "Missing successful terminal response");
		return NULL;
	}
	J *rows = json_object_new_array();
	const char *p = raw;
	size_t plen = strlen(prefix);
	while (*p) {
		const char *end = p + strcspn(p, "\r\n");
		while (p < end && isspace((unsigned char)*p))
			p++;
		const char *last = end;
		while (last > p && isspace((unsigned char)last[-1]))
			last--;
		if ((size_t)(last - p) >= plen && !memcmp(p, prefix, plen)) {
			const char *value = p + plen;
			while (value < last && isspace((unsigned char)*value))
				value++;
			J *row = csv(value, (size_t)(last - value));
			if (!row) {
				json_object_put(rows);
				return NULL;
			}
			json_object_array_add(rows, row);
		}
		p = end;
		while (*p == '\r' || *p == '\n')
			p++;
	}
	return rows;
}

J *at_one(const char *raw, const char *prefix)
{
	J *rows = at_fields(raw, prefix);
	if (!rows)
		return NULL;
	if (json_object_array_length(rows) != 1) {
		json_object_put(rows);
		errorf(CELL_INVALID, "Missing or ambiguous %s", prefix);
		return NULL;
	}
	J *row = json_object_get(json_object_array_get_idx(rows, 0));
	json_object_put(rows);
	return row;
}
