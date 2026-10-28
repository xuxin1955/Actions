/* SPDX-License-Identifier: GPL-2.0-only */
#include "cellular.h"
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *fields[] = {"name", "apn", "pdp_type", "username", "password", "auth_method"};

static bool credential(const char *s)
{
	if (!s || strlen(s) > 64)
		return false;
	for (const unsigned char *p = (const unsigned char *)s; *p; p++)
		if (*p < 32 || *p > 126 || *p == '"' || *p == '\\')
			return false;
	return true;
}

int apn_auth_validate(J *args)
{
	const char *method = str(args, "auth_method");
	if (!text_equal(method, "none") && !text_equal(method, "pap") &&
	    !text_equal(method, "chap"))
		return errorf(CELL_INVALID, "Unknown APN authentication method");
	if (!credential(str(args, "username")) ||
	    (get(args, "password") && !credential(str(args, "password"))))
		return errorf(CELL_INVALID, "Invalid APN credentials");
	return 0;
}

int apn_validate(const char *op, J *args)
{
	const char *const save[] = {"id",	"name",	    "apn",	  "pdp_type",
				    "username", "password", "auth_method"},
			  *const del[] = {"id"}, *const select[] = {"mode", "template_id"};
	if (!strcmp(op, "apn_templates"))
		return keys(args, NULL, 0) ? 0
					   : errorf(CELL_INVALID, "Unexpected APN query arguments");
	if (!strcmp(op, "apn_template_save") || !strcmp(op, "apn_template_delete")) {
		bool saving = !strcmp(op, "apn_template_save");
		if (!keys(args, saving ? save : del, saving ? 7 : 1) ||
		    !json_object_is_type(get(args, "id"), json_type_int) ||
		    json_object_get_int64(get(args, "id")) < (saving ? 0 : 1) ||
		    json_object_get_int64(get(args, "id")) > INT_MAX - 1)
			return errorf(CELL_INVALID, "Invalid APN template identifier");
		if (!saving)
			return 0;
		const char *name = str(args, "name");
		if (!name || !*name)
			return errorf(CELL_INVALID, "Invalid APN template name");
		size_t count = 0;
		for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
			if (*p < 32)
				return errorf(CELL_INVALID, "Invalid APN template name");
			if ((*p & 0xc0) != 0x80)
				count++;
		}
		if (count > 64)
			return errorf(CELL_INVALID, "APN template name is too long");
		J *p = json_object_new_object();
		json_object_object_add(p, "apn", json_object_get(get(args, "apn")));
		json_object_object_add(p, "pdp_type", json_object_get(get(args, "pdp_type")));
		integer(p, "cid", 1);
		int rc = profile_validate(p);
		json_object_put(p);
		if (rc < 0)
			return -1;
		if (!*str(args, "apn"))
			return errorf(CELL_INVALID, "Manual template needs an APN");
		return apn_auth_validate(args);
	}
	if (!strcmp(op, "apn_select")) {
		const char *mode = str(args, "mode");
		int64_t id = json_object_get_int64(get(args, "template_id"));
		if (!keys(args, select, 2) ||
		    (!text_equal(mode, "auto") && !text_equal(mode, "manual")) ||
		    !json_object_is_type(get(args, "template_id"), json_type_int) || id < 0 ||
		    id > INT_MAX - 1 || text_equal(mode, "auto") != (id == 0))
			return errorf(CELL_INVALID, "Select auto or a manual APN template");
		return 0;
	}
	return errorf(CELL_INVALID, "Unknown APN operation");
}

J *apn_public_auth(J *config)
{
	J *out = json_object_new_object();
	string(out, "username", str(config, "username"));
	string(out, "auth_method", str(config, "auth_method"));
	const char *p = str(config, "password");
	boolean(out, "password_set", p && *p);
	return out;
}

static int compare_id(const void *a, const void *b)
{
	J *const *x = a, *const *y = b;
	int64_t first = json_object_get_int64(get(*x, "id")),
		second = json_object_get_int64(get(*y, "id"));
	return (first > second) - (first < second);
}

static J *catalog(void)
{
	J *sections = cell_config_sections(), *rows = json_object_new_array();
	if (!sections) {
		json_object_put(rows);
		return NULL;
	}
	int64_t next = 1;
	const char *n = str(get(sections, "modem"), "template_next_id");
	if (n && !integer_text(n, 1, INT_MAX - 1, &next))
		goto invalid;
	json_object_object_foreach(sections, name, values)
	{
		if (!text_equal(str(values, ".type"), "apn_template"))
			continue;
		int64_t id;
		if (strncmp(name, "apn_", 4) || !integer_text(name + 4, 1, INT_MAX - 2, &id))
			goto invalid;
		J *row = json_object_new_object();
		for (size_t i = 0; i < 6; i++)
			string(row, fields[i],
			       str(values, fields[i]) ? str(values, fields[i]) : "");
		integer(row, "id", id);
		if (apn_validate("apn_template_save", row) < 0) {
			json_object_put(row);
			goto fail;
		}
		json_object_array_add(rows, row);
		if (id >= next)
			next = id + 1;
	}
	if (json_object_array_length(rows) > 64)
		goto invalid;
	json_object_array_sort(rows, compare_id);
	J *out = json_object_new_object();
	integer(out, "next_id", next);
	json_object_object_add(out, "templates", rows);
	json_object_put(sections);
	return out;
invalid:
	errorf(CELL_INVALID, "Invalid APN template catalog");
fail:
	json_object_put(rows);
	json_object_put(sections);
	return NULL;
}

J *apn_publish(struct controller *c)
{
	J *data = catalog();
	if (!data)
		return NULL;
	J *rows = get(data, "templates");
	for (size_t i = 0; i < json_object_array_length(rows); i++) {
		J *row = json_object_array_get_idx(rows, i);
		boolean(row, "password_set", str(row, "password") && *str(row, "password"));
		json_object_object_del(row, "password");
	}
	J *out = json_object_new_object();
	string(out, "boot_id", c->transport.boot);
	real(out, "uptime", mono());
	string(out, "mode", str(c->config, "apn_mode"));
	integer(out, "selected_template_id", strtoll(str(c->config, "apn_template_id"), NULL, 10));
	json_object_object_add(out, "templates", json_object_get(rows));
	json_object_object_add(out, "profile", controller_profile(c));
	json_object_object_add(out, "auth", apn_public_auth(c->config));
	json_object_put(data);
	if (atomic_json(CELL_RUN "/apn-status.json", out, false) < 0) {
		json_object_put(out);
		return NULL;
	}
	return out;
}

int apn_configure(struct controller *c)
{
	char command[320];
	J *profile = c->fallback_profile ? c->fallback_profile : c->config;
	snprintf(command, sizeof(command), "AT+CGDCONT=1,\"%s\",\"%s\"", str(profile, "pdp_type"),
		 str(profile, "apn"));
	char *raw = controller_at(c, command, 5);
	if (!raw)
		return -1;
	free(raw);
	const char *method = str(profile, "auth_method");
	int auth = text_equal(method, "pap") ? 1 : text_equal(method, "chap") ? 2 : 0;
	snprintf(command, sizeof(command), "AT+CGPCO=0,\"%s\",\"%s\",1,%d",
		 str(profile, "username"), str(profile, "password"), auth);
	raw = controller_at(c, command, 5);
	if (!raw)
		return -1;
	free(raw);
	return 0;
}

static int set_config(struct controller *c, J *values)
{
	J *changes = json_object_new_object();
	json_object_object_foreach(values, key, value)
	{
		char name[80];
		snprintf(name, sizeof(name), "modem.%s", key);
		json_object_object_add(changes, name, json_object_get(value));
	}
	int rc = cell_config_commit(changes, NULL);
	json_object_put(changes);
	if (rc < 0)
		return -1;
	json_object_object_foreach(values, key2, value2)
	{
		string(c->config, key2, json_object_get_string(value2));
	}
	if (controller_publish(c) < 0)
		return -1;
	J *p = apn_publish(c);
	if (!p)
		return -1;
	json_object_put(p);
	return 0;
}

static J *apply(struct controller *c, J *values)
{
	c->fallback_attempted = false;
	c->fallback_index = 0;
	c->fallback_tried[0] = 0;
	json_object_put(c->fallback_profile);
	c->fallback_profile = NULL;
	json_object_object_del(c->status, "automatic_apn");
	bool wanted = c->want_data;
	c->want_data = false;
	J *disconnected = controller_disconnect(c, false);
	if (!disconnected) {
		if (!c->transport.poisoned)
			c->want_data = wanted;
		return NULL;
	}
	json_object_put(disconnected);
	int rc = set_config(c, values);
	if (!c->transport.poisoned)
		c->want_data = wanted;
	if (rc < 0)
		return NULL;
	char *sim = controller_sim(c);
	if (!sim)
		return NULL;
	bool ready = !strcmp(sim, "READY");
	free(sim);
	J *result = NULL;
	if (!ready) {
		c->want_data = wanted;
		if (controller_publish(c) < 0)
			return NULL;
		result = json_object_new_object();
		boolean(result, "saved", true);
		boolean(result, "applied", false);
		string(result, "waiting_for", "SIM");
	} else {
		c->want_data = wanted;
		if (wanted)
			result = controller_connect(c, 10);
		else {
			if (apn_configure(c) < 0)
				return NULL;
			result = json_object_new_object();
			boolean(result, "connected", false);
		}
		if (!result)
			return NULL;
		boolean(result, "saved", true);
		boolean(result, "applied", true);
		boolean(result, "command_accepted", true);
	}
	J *published = apn_publish(c);
	if (!published) {
		json_object_put(result);
		return NULL;
	}
	json_object_object_add(result, "apn", published);
	return result;
}

J *apn_direct(struct controller *c, J *args)
{
	J *values = jclone(args);
	string(values, "apn_mode", "manual");
	string(values, "apn_template_id", "0");
	J *pass = NULL;
	if (json_object_object_get_ex(values, "password", &pass) && !pass)
		string(values, "password", str(c->config, "password"));
	J *out = apply(c, values);
	json_object_put(values);
	return out;
}

J *apn_execute(struct controller *c, const char *op, J *args)
{
	if (!strcmp(op, "apn_templates"))
		return apn_publish(c);
	J *data = catalog();
	if (!data)
		return NULL;
	J *rows = get(data, "templates"), *selected = NULL;
	int64_t id =
	    json_object_get_int64(get(args, "id") ? get(args, "id") : get(args, "template_id"));
	for (size_t i = 0; i < json_object_array_length(rows); i++) {
		J *r = json_object_array_get_idx(rows, i);
		if (json_object_get_int64(get(r, "id")) == id) {
			selected = r;
			break;
		}
	}
	J *result = NULL, *values = NULL;
	if (!strcmp(op, "apn_template_save")) {
		if (id && !selected) {
			errorf(CELL_INVALID, "APN template no longer exists");
			goto end;
		}
		if (!selected && json_object_array_length(rows) >= 64) {
			errorf(CELL_INVALID, "At most 64 APN templates");
			goto end;
		}
		int64_t next = json_object_get_int64(get(data, "next_id"));
		if (!selected) {
			id = next++;
		}
		char section[48];
		snprintf(section, sizeof(section), "apn_%lld", (long long)id);
		values = json_object_new_object();
		string(values, section, "apn_template");
		integer(values, "modem.template_next_id", next);
		for (size_t i = 0; i < 6; i++) {
			char key[96];
			snprintf(key, sizeof(key), "%s.%s", section, fields[i]);
			const char *v = str(args, fields[i]);
			if (i == 4 && !v)
				v = selected ? str(selected, "password") : "";
			string(values, key, v);
		}
		if (cell_config_commit(values, NULL) < 0)
			goto end;
		result = json_object_new_object();
		integer(result, "saved_id", id);
	} else if (!strcmp(op, "apn_template_delete")) {
		if (id == strtoll(str(c->config, "apn_template_id"), NULL, 10)) {
			errorf(CELL_INVALID, "Select another APN before deleting this template");
			goto end;
		}
		if (!selected) {
			errorf(CELL_INVALID, "APN template no longer exists");
			goto end;
		}
		char section[48];
		snprintf(section, sizeof(section), "apn_%lld", (long long)id);
		J *deletes = json_object_new_array();
		json_object_array_add(deletes, json_object_new_string(section));
		int rc = cell_config_commit(NULL, deletes);
		json_object_put(deletes);
		if (rc < 0)
			goto end;
		result = json_object_new_object();
		integer(result, "deleted_id", id);
	} else {
		values = json_object_new_object();
		if (text_equal(str(args, "mode"), "auto")) {
			string(values, "apn", "");
			string(values, "pdp_type", "IPV4V6");
			string(values, "username", "");
			string(values, "password", "");
			string(values, "auth_method", "none");
			string(values, "apn_mode", "auto");
			string(values, "apn_template_id", "0");
		} else {
			if (!selected) {
				errorf(CELL_INVALID, "Select an existing APN template");
				goto end;
			}
			for (size_t i = 1; i < 6; i++)
				string(values, fields[i], str(selected, fields[i]));
			string(values, "apn_mode", "manual");
			char number[32];
			snprintf(number, sizeof(number), "%lld", (long long)id);
			string(values, "apn_template_id", number);
		}
		string(values, "cid", "1");
		result = apply(c, values);
		goto end;
	}
	if (result) {
		J *published = apn_publish(c);
		if (!published) {
			json_object_put(result);
			result = NULL;
		} else
			json_object_object_add(result, "apn", published);
	}
end:
	json_object_put(values);
	json_object_put(data);
	return result;
}
