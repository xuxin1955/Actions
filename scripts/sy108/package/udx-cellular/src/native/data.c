/* SPDX-License-Identifier: GPL-2.0-only */
#include "cellular.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

bool apn_matches(const char *wanted, const char *actual)
{
	if (!wanted || !actual)
		return false;
	if (!*wanted || !strcasecmp(wanted, actual))
		return true;
	size_t n = strlen(wanted);
	if (strncasecmp(wanted, actual, n))
		return false;
	const char *s = actual + n;
	return strlen(s) == 19 && !strncasecmp(s, ".mnc", 4) && isdigit((unsigned char)s[4]) &&
	       isdigit((unsigned char)s[5]) && isdigit((unsigned char)s[6]) &&
	       !strncasecmp(s + 7, ".mcc", 4) && isdigit((unsigned char)s[11]) &&
	       isdigit((unsigned char)s[12]) && isdigit((unsigned char)s[13]) &&
	       !strcasecmp(s + 14, ".gprs");
}

int profile_validate(J *p)
{
	const char *const names[] = {"apn", "pdp_type", "cid"};
	const char *a = str(p, "apn"), *t = str(p, "pdp_type");
	if (!keys(p, names, 3) || !a || !t ||
	    json_object_get_type(get(p, "cid")) != json_type_int ||
	    json_object_get_int64(get(p, "cid")) != 1)
		return errorf(CELL_INVALID, "Invalid APN profile");
	if (strlen(a) > 100 || (*a && !isalnum((unsigned char)*a)))
		return errorf(CELL_INVALID, "Invalid APN");
	for (const unsigned char *s = (const unsigned char *)a; *s; s++)
		if (*s > 127 || (!isalnum(*s) && *s != '.' && *s != '-'))
			return errorf(CELL_INVALID, "Invalid APN");
	if (strcmp(t, "IP") && strcmp(t, "IPV6") && strcmp(t, "IPV4V6"))
		return errorf(CELL_INVALID, "Invalid PDP type");
	return 0;
}

struct address {
	unsigned char bytes[16];
	int family;
};

static int address_mask(const char *value, struct address *a)
{
	char copy[512];
	if (!value || strlen(value) >= sizeof(copy))
		return errorf(CELL_INVALID, "Invalid PDP address");
	strcpy(copy, value);
	char *save = NULL, *first = strtok_r(copy, " \t", &save),
	     *second = strtok_r(NULL, " \t", &save);
	if (!first || strtok_r(NULL, " \t", &save))
		return errorf(CELL_INVALID, "Unexpected PDP address shape");
	struct address mask = {0};
	bool has_mask = false, octets = !second;
	for (char *s = first; *s; s++)
		if (!isdigit((unsigned char)*s) && *s != '.')
			octets = false;
	memset(a, 0, sizeof(*a));
	if (octets) {
		unsigned char bytes[32];
		size_t n = 0;
		char *s = first;
		while (*s) {
			char *end;
			unsigned long v = strtoul(s, &end, 10);
			if (end == s || v > 255 || n == 32 || (*end && *end != '.') ||
			    (*end == '.' && !end[1]))
				goto invalid;
			bytes[n++] = (unsigned char)v;
			s = *end ? end + 1 : end;
		}
		if (n != 4 && n != 8 && n != 16 && n != 32)
			goto invalid;
		has_mask = n == 8 || n == 32;
		size_t size = has_mask ? n / 2 : n;
		a->family = size == 4 ? AF_INET : AF_INET6;
		memcpy(a->bytes, bytes, size);
		if (has_mask) {
			mask.family = a->family;
			memcpy(mask.bytes, bytes + size, size);
		}
	} else {
		a->family = strchr(first, ':') ? AF_INET6 : AF_INET;
		if (inet_pton(a->family, first, a->bytes) != 1)
			goto invalid;
		if (second) {
			mask.family = strchr(second, ':') ? AF_INET6 : AF_INET;
			if (mask.family != a->family ||
			    inet_pton(mask.family, second, mask.bytes) != 1)
				goto invalid;
			has_mask = true;
		}
	}
	if (has_mask) {
		bool zero = false;
		for (int i = 0; i < (a->family == AF_INET ? 4 : 16); i++)
			for (int b = 7; b >= 0; b--) {
				bool one = mask.bytes[i] & (1u << b);
				if (zero && one)
					return errorf(CELL_INVALID,
						      "Non-contiguous PDP subnet mask");
				if (!one)
					zero = true;
			}
	}
	return 0;
invalid:
	return errorf(CELL_INVALID, "Invalid PDP address or mask");
}

static bool linklocal(struct address *a)
{
	return a->family == AF_INET ? (a->bytes[0] == 169 && a->bytes[1] == 254)
				    : (a->bytes[0] == 0xfe && (a->bytes[1] & 0xc0) == 0x80);
}

static bool usable(struct address *a)
{
	if (a->family == AF_INET) {
		uint32_t v;
		memcpy(&v, a->bytes, 4);
		return v && a->bytes[0] != 127 && (a->bytes[0] & 0xf0) != 0xe0;
	}
	static const unsigned char zero[16] = {0}, loop[16] = {[15] = 1};
	return memcmp(a->bytes, zero, 16) && memcmp(a->bytes, loop, 16) && a->bytes[0] != 0xff;
}

static bool contains(J *a, const char *s)
{
	for (size_t i = 0; i < json_object_array_length(a); i++)
		if (text_equal(json_object_get_string(json_object_array_get_idx(a, i)), s))
			return true;
	return false;
}

J *parse_pdp(const char *raw, J *profile)
{
	if (profile_validate(profile) < 0)
		return NULL;
	J *rows = at_fields(raw, "+CGCONTRDP:"), *out = NULL;
	if (!rows)
		return NULL;
	out = jclone(profile);
	J *dns = json_object_new_array();
	json_object_object_add(out, "dns", dns);
	for (size_t i = 0; i < json_object_array_length(rows); i++) {
		J *r = json_object_array_get_idx(rows, i);
		if (json_object_array_length(r) < 7 ||
		    !text_equal(json_object_get_string(json_object_array_get_idx(r, 0)), "1"))
			continue;
		const char *apn = json_object_get_string(json_object_array_get_idx(r, 2));
		if (!apn_matches(str(profile, "apn"), apn)) {
			errorf(CELL_INVALID, "APN readback differs from requested profile");
			goto fail;
		}
		string(out, "network_apn", apn);
		struct address a;
		if (address_mask(json_object_get_string(json_object_array_get_idx(r, 3)), &a) < 0)
			goto fail;
		char value[INET6_ADDRSTRLEN];
		if (usable(&a)) {
			bool low = a.family == AF_INET6;
			for (int k = 0; k < 8; k++)
				if (a.bytes[k])
					low = false;
			if (low) {
				a.bytes[0] = 0xfe;
				a.bytes[1] = 0x80;
			}
			inet_ntop(a.family, a.bytes, value, sizeof(value));
			string(out,
			       a.family == AF_INET
				   ? "ipv4"
				   : (low || linklocal(&a) ? "ipv6_linklocal" : "ipv6"),
			       value);
		}
		for (size_t k = 5; k < 7; k++) {
			const char *d = json_object_get_string(json_object_array_get_idx(r, k));
			if (!*d)
				continue;
			if (address_mask(d, &a) < 0)
				goto fail;
			if (usable(&a) && !linklocal(&a)) {
				inet_ntop(a.family, a.bytes, value, sizeof(value));
				if (!contains(dns, value))
					json_object_array_add(dns, json_object_new_string(value));
			}
		}
	}
	if (!get(out, "ipv4") && !get(out, "ipv6") && !get(out, "ipv6_linklocal")) {
		errorf(CELL_INVALID, "PDP has no usable negotiated address");
		goto fail;
	}
	json_object_put(rows);
	return out;
fail:
	json_object_put(rows);
	json_object_put(out);
	return NULL;
}

int bearer_active(const char *raw)
{
	J *rows = at_fields(raw, "+CGACT:");
	if (!rows)
		return -1;
	int result = 0, count = 0;
	for (size_t i = 0; i < json_object_array_length(rows); i++) {
		J *r = json_object_array_get_idx(rows, i);
		if (!text_equal(json_object_get_string(json_object_array_get_idx(r, 0)), "1"))
			continue;
		const char *v = json_object_get_string(json_object_array_get_idx(r, 1));
		if (++count > 1 || json_object_array_length(r) != 2 ||
		    (!text_equal(v, "0") && !text_equal(v, "1"))) {
			json_object_put(rows);
			return errorf(CELL_INVALID, "Invalid CID 1 activation readback");
		}
		result = !strcmp(v, "1");
	}
	json_object_put(rows);
	return result;
}

J *bearer_default(const char *raw, J *profile)
{
	J *rows = at_fields(raw, "+SPIPCONTEXT:"), *selected = NULL, *out = NULL;
	if (!rows)
		return NULL;
	for (size_t i = 0; i < json_object_array_length(rows); i++) {
		J *r = json_object_array_get_idx(rows, i);
		if (text_equal(json_object_get_string(json_object_array_get_idx(r, 0)), "1")) {
			if (selected) {
				selected = NULL;
				break;
			}
			selected = r;
		}
	}
	if (!selected || json_object_array_length(selected) < 4)
		goto fail;
	const char *state = json_object_get_string(json_object_array_get_idx(selected, 1)),
		   *apn = json_object_get_string(json_object_array_get_idx(selected, 2)),
		   *kind = json_object_get_string(json_object_array_get_idx(selected, 3));
	int64_t type;
	if (!text_equal(state, "1") || !integer_text(kind, 1, 4, &type))
		goto fail;
	const char *types[] = {"IP", "IPV6", "IPV4V6", "ETHERNET"};
	if (!text_equal(types[type - 1], str(profile, "pdp_type")) ||
	    !apn_matches(str(profile, "apn"), apn))
		goto fail;
	out = json_object_new_object();
	integer(out, "cid", 1);
	string(out, "apn", apn);
	string(out, "pdp_type", types[type - 1]);
	string(out, "source", "SPIPCONTEXT");
	json_object_put(rows);
	return out;
fail:
	json_object_put(rows);
	errorf(CELL_INVALID, "Default PDN identity or APN/protocol does not match");
	return NULL;
}
