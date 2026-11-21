/* SPDX-License-Identifier: GPL-2.0-only */
#include "cellular.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static J *load(void)
{
	char *raw = read_text(CELL_STORE "/sms.json", 2097152, false);
	int saved = errno;
	J *data = raw ? parse_json_document(raw, strlen(raw)) : NULL;
	if (raw && !data)
		saved = EINVAL;
	if (!raw && saved != ENOENT)
		errorf(CELL_SYSTEM, "Read SMS history: %s", strerror(saved));
	free(raw);
	if (!data && saved == ENOENT) {
		data = json_object_new_object();
		json_object_object_add(data, "inbox", json_object_new_array());
		json_object_object_add(data, "sent", json_object_new_array());
	}
	const char *const names[] = {"inbox", "sent"};
	if (!data)
		return NULL;
	if (!keys(data, names, 2) || !json_object_is_type(get(data, "inbox"), json_type_array) ||
	    !json_object_is_type(get(data, "sent"), json_type_array)) {
		json_object_put(data);
		errorf(CELL_INVALID, "Invalid SMS history file");
		return NULL;
	}
	for (size_t f = 0; f < 2; f++) {
		J *rows = get(data, names[f]);
		for (size_t i = 0; i < json_object_array_length(rows); i++) {
			J *row = json_object_array_get_idx(rows, i);
			if (!hex_string(str(row, "id"), f ? 32 : 64)) {
				json_object_put(data);
				errorf(CELL_INVALID, "Invalid SMS history identity");
				return NULL;
			}
		}
	}
	return data;
}

int sms_settings_validate(J *value)
{
	const char *const names[] = {"max_count", "max_sent_count", "direct_receive"};
	if (!keys(value, names, 3) ||
	    !json_object_is_type(get(value, "max_count"), json_type_int) ||
	    !json_object_is_type(get(value, "max_sent_count"), json_type_int) ||
	    !json_object_is_type(get(value, "direct_receive"), json_type_boolean))
		return errorf(CELL_INVALID, "Invalid SMS settings");
	int64_t in = json_object_get_int64(get(value, "max_count")),
		sent = json_object_get_int64(get(value, "max_sent_count"));
	if (in < 10 || in > 150 || sent < 1 || sent > 50)
		return errorf(CELL_INVALID, "Invalid SMS history limits");
	return 0;
}

J *sms_configuration(void)
{
	J *sections = cell_config_sections();
	if (!sections)
		return NULL;
	J *row = get(sections, "sms"), *value = json_object_new_object();
	int64_t in = 50, sent = 10;
	const char *s = str(row, "max_count");
	if (s && !integer_text(s, 10, 150, &in))
		goto fail;
	s = str(row, "max_sent_count");
	if (s && !integer_text(s, 1, 50, &sent))
		goto fail;
	const char *d = str(row, "direct_receive");
	if (d && strcmp(d, "0") && strcmp(d, "1"))
		goto fail;
	integer(value, "max_count", in);
	integer(value, "max_sent_count", sent);
	boolean(value, "direct_receive", text_equal(d, "1"));
	json_object_put(sections);
	return value;
fail:
	json_object_put(value);
	json_object_put(sections);
	errorf(CELL_INVALID, "Invalid SMS configuration");
	return NULL;
}

static int save(J *data)
{
	J *config = sms_configuration();
	if (!config)
		return -1;
	const char *names[] = {"inbox", "sent"}, *limits[] = {"max_count", "max_sent_count"};
	for (size_t i = 0; i < 2; i++) {
		J *a = get(data, names[i]);
		size_t n = json_object_array_length(a),
		       max = (size_t)json_object_get_int(get(config, limits[i]));
		if (n > max)
			json_object_array_del_idx(a, 0, n - max);
	}
	json_object_put(config);
	if (mkdir_private(CELL_STORE) < 0)
		return -1;
	return atomic_json(CELL_STORE "/sms.json", data, true);
}

struct group {
	const char *peer;
	int reference, total;
	time_t stamp;
	J *row;
	J *parts[256];
};

J *sms_inbox(J *rows)
{
	size_t n = json_object_array_length(rows), used = 0;
	struct group *groups = calloc(n ? n : 1, sizeof(*groups));
	J *messages = json_object_new_array();
	if (!groups) {
		json_object_put(messages);
		errorf(CELL_SYSTEM, "SMS group allocation failed");
		return NULL;
	}
	for (size_t i = 0; i < n; i++) {
		J *row = jclone(json_object_array_get_idx(rows, i));
		json_object_object_del(row, "raw_pdu");
		J *concat = get(row, "concat");
		int total = json_object_get_int(get(concat, "total")),
		    part = json_object_get_int(get(concat, "part"));
		struct tm tm = {0};
		const char *stamp = str(row, "timestamp"), *end = NULL;
		if (stamp)
			end = strptime(stamp, "%Y-%m-%d %H:%M:%S", &tm);
		if (!concat || !text_equal(str(row, "kind"), "received") || part < 1 ||
		    part > total || total > 255 || !end || *end) {
			json_object_array_add(messages, row);
			continue;
		}
		time_t timestamp = timegm(&tm);
		struct group *g = NULL;
		int ref = json_object_get_int(get(concat, "reference"));
		for (size_t j = used; j > 0; j--) {
			struct group *test = &groups[j - 1];
			if (text_equal(test->peer, str(row, "peer")) && test->reference == ref &&
			    test->total == total &&
			    llabs((long long)timestamp - test->stamp) <= 600 &&
			    !test->parts[part]) {
				g = test;
				break;
			}
		}
		if (!g) {
			g = &groups[used++];
			g->row = row;
			g->peer = str(row, "peer");
			g->reference = ref;
			g->total = total;
			g->stamp = timestamp;
			json_object_array_add(messages, row);
		}
		g->parts[part] = jclone(row);
		if (g->row != row)
			json_object_put(row);
	}
	for (size_t i = 0; i < used; i++) {
		struct group *g = &groups[i];
		J *ids = json_object_new_array();
		bool read = true;
		size_t count = 0, capacity = 1, used_text = 0;
		for (int p = 1; p <= g->total; p++) {
			J *r = g->parts[p];
			capacity += r && str(r, "text")
					? (size_t)json_object_get_string_len(get(r, "text")) + 80
					: 80;
		}
		char *text = calloc(capacity, 1);
		if (!text) {
			for (size_t j = 0; j < used; j++)
				for (int k = 1; k <= groups[j].total; k++)
					json_object_put(groups[j].parts[k]);
			free(groups);
			json_object_put(ids);
			json_object_put(messages);
			errorf(CELL_SYSTEM, "SMS text allocation failed");
			return NULL;
		}
		for (int p = 1; p <= g->total; p++) {
			J *r = g->parts[p];
			const char *s;
			size_t length;
			char missing[80];
			if (r) {
				count++;
				json_object_array_add(ids, json_object_get(get(r, "id")));
				read &= json_object_get_boolean(get(r, "read"));
				s = str(r, "text");
				length = s ? (size_t)json_object_get_string_len(get(r, "text")) : 0;
				if (!length) {
					s = "[This message part could not be decoded]";
					length = strlen(s);
				}
			} else {
				snprintf(missing, sizeof(missing), "[Missing part %d]", p);
				s = missing;
				length = strlen(s);
			}
			memcpy(text + used_text, s, length);
			used_text += length;
		}
		json_object_object_add(g->row, "part_ids", ids);
		boolean(g->row, "read", read);
		integer(get(g->row, "concat"), "received", (int64_t)count);
		boolean(get(g->row, "concat"), "complete", count == (size_t)g->total);
		json_object_object_add(g->row, "text",
				       json_object_new_string_len(text, (int)used_text));
		free(text);
		for (int p = 1; p <= g->total; p++)
			json_object_put(g->parts[p]);
	}
	free(groups);
	return messages;
}

J *sms_publish(struct controller *c)
{
	J *data = load(), *settings = NULL, *inbox = NULL, *value = NULL;
	if (!data)
		return NULL;
	settings = sms_configuration();
	if (!settings)
		goto fail;
	inbox = sms_inbox(get(data, "inbox"));
	if (!inbox)
		goto fail;
	J *sent = get(data, "sent");
	for (size_t i = 0; i < json_object_array_length(sent); i++) {
		J *row = json_object_array_get_idx(sent, i);
		if (text_equal(str(row, "state"), "sending") &&
		    (!text_equal(str(row, "sender_instance"), c->sms_instance) ||
		     !text_equal(str(row, "id"), c->sms_active_request))) {
			string(row, "state", "uncertain");
			string(row, "error",
			       "Sender ended before final confirmation; no automatic replay");
		}
		json_object_object_del(row, "sender_instance");
	}
	value = json_object_new_object();
	string(value, "boot_id", c->transport.boot);
	real(value, "uptime", mono());
	json_object_object_add(value, "settings", settings);
	settings = NULL;
	json_object_object_add(value, "inbox", inbox);
	inbox = NULL;
	json_object_object_add(value, "sent", json_object_get(sent));
	if (atomic_json(CELL_RUN "/sms-history.json", value, false) < 0) {
		json_object_put(value);
		value = NULL;
	}
fail:
	json_object_put(settings);
	json_object_put(inbox);
	json_object_put(data);
	return value;
}

static int archive(const char *pdu, const char *source, J *index)
{
	char *upper = strdup(pdu);
	if (!upper)
		return errorf(CELL_SYSTEM, "SMS allocation failed");
	for (char *s = upper; *s; s++)
		*s = (char)toupper((unsigned char)*s);
	char digest[65];
	int hash_error = sha256_hex(upper, strlen(upper), digest);
	free(upper);
	if (hash_error)
		return -1;
	J *data = load();
	if (!data)
		return -1;
	J *rows = get(data, "inbox");
	for (size_t i = 0; i < json_object_array_length(rows); i++)
		if (text_equal(str(json_object_array_get_idx(rows, i), "id"), digest)) {
			json_object_put(data);
			return 0;
		}
	J *decoded = sms_decode(pdu);
	if (!decoded) {
		decoded = json_object_new_object();
		string(decoded, "kind", "unknown");
		string(decoded, "decode_error", cell_error);
	}
	J *row = json_object_new_object();
	const char *names[] = {"peer", "text", "timestamp", "concat", "kind", "decode_error"};
	for (size_t i = 0; i < 6; i++)
		json_object_object_add(row, names[i], json_object_get(get(decoded, names[i])));
	string(row, "id", digest);
	integer(row, "received_at", time(NULL));
	boolean(row, "read", false);
	string(row, "source", source);
	string(row, "raw_pdu", pdu);
	if (index)
		json_object_object_add(row, "modem_index", json_object_get(index));
	json_object_array_add(rows, row);
	json_object_put(decoded);
	int rc = save(data);
	json_object_put(data);
	return rc;
}

int sms_configure(struct controller *c, J *settings)
{
	J *value = settings ? json_object_get(settings) : sms_configuration();
	if (!value)
		return -1;
	int mode = json_object_get_boolean(get(value, "direct_receive")) ? 2 : 1;
	json_object_put(value);
	char command[64];
	snprintf(command, sizeof(command), "AT+CNMI=3,%d,0,1,0", mode);
	const char *commands[] = {"AT+CMGF=0", "AT+CSMS=1", command};
	for (size_t i = 0; i < 3; i++) {
		char *r = controller_at(c, commands[i], i ? 10 : 5);
		if (!r)
			return -1;
		free(r);
	}
	J *r = controller_query(c, "AT+CNMI?", 5, "+CNMI:");
	if (!r)
		return -1;
	bool valid = json_object_array_length(r) == 5;
	const char *wanted[] = {"3", mode == 2 ? "2" : "1", "0", "1", "0"};
	for (size_t i = 0; i < 5; i++)
		valid &=
		    text_equal(json_object_get_string(json_object_array_get_idx(r, i)), wanted[i]);
	json_object_put(r);
	return valid ? 0 : errorf(CELL_MODEM, "SMS receive settings readback mismatch");
}

J *sms_refresh(struct controller *c)
{
	J *r = controller_query(c, "AT+CMGF?", 5, "+CMGF:");
	if (!r)
		return NULL;
	bool valid = json_object_array_length(r) == 1 &&
		     text_equal(json_object_get_string(json_object_array_get_idx(r, 0)), "0");
	json_object_put(r);
	if (!valid) {
		errorf(CELL_MODEM, "SMS PDU mode is not active");
		return NULL;
	}
	char *raw = controller_at(c, "AT+CMGL=4", 30);
	if (!raw)
		return NULL;
	J *rows = sms_list(raw);
	free(raw);
	if (!rows)
		return NULL;
	for (size_t i = 0; i < json_object_array_length(rows); i++) {
		J *item = json_object_array_get_idx(rows, i);
		const char *pdu = str(item, "raw_pdu");
		if (text_equal(str(item, "kind"), "received") ||
		    text_equal(str(item, "kind"), "status_report"))
			if (archive(pdu, "modem_storage", get(item, "index")) < 0) {
				json_object_put(rows);
				return NULL;
			}
		char *upper = strdup(pdu);
		if (!upper) {
			json_object_put(rows);
			errorf(CELL_SYSTEM, "SMS allocation failed");
			return NULL;
		}
		for (char *s = upper; *s; s++)
			*s = (char)toupper((unsigned char)*s);
		char digest[65];
		int hash_error = sha256_hex(upper, strlen(upper), digest);
		free(upper);
		if (hash_error) {
			json_object_put(rows);
			return NULL;
		}
		string(item, "pdu_sha256", digest);
		json_object_object_del(item, "raw_pdu");
	}
	J *published = sms_publish(c);
	if (!published) {
		json_object_put(rows);
		return NULL;
	}
	json_object_put(published);
	J *value = json_object_new_object();
	string(value, "boot_id", c->transport.boot);
	real(value, "uptime", mono());
	json_object_object_add(value, "messages", rows);
	if (atomic_json(CELL_RUN "/sms-status.json", value, false) < 0) {
		json_object_put(value);
		return NULL;
	}
	return value;
}

int sms_process_events(struct controller *c)
{
	J *events = c->transport.sms_events;
	size_t n = json_object_array_length(events);
	if (!n)
		return 0;
	if (n > 128)
		n = 128;
	for (size_t i = 0; i < n; i++) {
		J *event = json_object_array_get_idx(events, 0);
		const char *pdu = str(event, "pdu");
		if (pdu) {
			if (archive(pdu, "direct", NULL) < 0)
				return -1;
			json_object_array_del_idx(events, 0, 1);
			char *r = controller_at(c, "AT+CNMA=1", 10);
			if (!r)
				return -1;
			free(r);
		} else {
			const char *storage = str(event, "storage");
			if (!storage || (!text_equal(storage, "SM") && !text_equal(storage, "ME") &&
					 !text_equal(storage, "MT")))
				return errorf(CELL_INVALID, "Invalid SMS storage event");
			char command[32];
			snprintf(command, sizeof(command), "AT+CPMS=\"%s\"", storage);
			char *r = controller_at(c, command, 10);
			if (!r)
				return -1;
			free(r);
			J *value = sms_refresh(c);
			if (!value)
				return -1;
			json_object_put(value);
			json_object_array_del_idx(events, 0, 1);
		}
	}
	J *published = sms_publish(c);
	if (!published)
		return -1;
	json_object_put(published);
	return 0;
}

static int sent_update(struct controller *c, const char *id, J *updates)
{
	J *data = load();
	if (!data)
		return -1;
	J *sent = get(data, "sent"), *selected = NULL;
	for (size_t i = 0; i < json_object_array_length(sent); i++) {
		J *row = json_object_array_get_idx(sent, i);
		if (text_equal(str(row, "id"), id)) {
			selected = row;
			break;
		}
	}
	if (!selected) {
		json_object_put(data);
		return errorf(CELL_INVALID, "SMS request is missing from history");
	}
	merge(selected, updates);
	int rc = save(data);
	json_object_put(data);
	if (rc < 0)
		return -1;
	J *value = sms_publish(c);
	if (!value)
		return -1;
	json_object_put(value);
	return 0;
}

static int sent_start(struct controller *c, J *args)
{
	if (!*c->sms_instance && random_hex(c->sms_instance, 16) < 0)
		return -1;
	J *data = load();
	if (!data)
		return -1;
	J *sent = get(data, "sent");
	const char *id = str(args, "request_id");
	for (size_t i = 0; i < json_object_array_length(sent); i++)
		if (text_equal(str(json_object_array_get_idx(sent, i), "id"), id)) {
			json_object_put(data);
			return errorf(CELL_INVALID, "SMS request already exists; no replay");
		}
	J *row = json_object_new_object();
	string(row, "id", id);
	string(row, "number", str(args, "number"));
	string(row, "text", str(args, "text"));
	integer(row, "timestamp", time(NULL));
	string(row, "state", "sending");
	json_object_object_add(row, "parts", json_object_new_array());
	string(row, "error", NULL);
	boolean(row, "delivery_confirmed", false);
	string(row, "sender_instance", c->sms_instance);
	snprintf(c->sms_active_request, sizeof(c->sms_active_request), "%s", id);
	json_object_array_add(sent, row);
	int rc = save(data);
	json_object_put(data);
	if (rc < 0)
		return -1;
	J *value = sms_publish(c);
	if (!value)
		return -1;
	json_object_put(value);
	return 0;
}

static J *summary(struct controller *c)
{
	J *value = sms_publish(c);
	if (!value)
		return NULL;
	J *out = json_object_new_object();
	boolean(out, "refreshed", true);
	json_object_object_add(out, "settings", json_object_get(get(value, "settings")));
	integer(out, "inbox_count", json_object_array_length(get(value, "inbox")));
	integer(out, "sent_count", json_object_array_length(get(value, "sent")));
	json_object_put(value);
	return out;
}

static bool id_in(J *ids, const char *id)
{
	for (size_t i = 0; i < json_object_array_length(ids); i++)
		if (text_equal(json_object_get_string(json_object_array_get_idx(ids, i)), id))
			return true;
	return false;
}

static J *history_execute(struct controller *c, const char *operation, J *args)
{
	if (!strcmp(operation, "sms_history"))
		return summary(c);
	J *data = load();
	if (!data)
		return NULL;
	if (!strcmp(operation, "sms_settings")) {
		if (sms_configure(c, args) < 0)
			goto fail;
		J *values = json_object_new_object();
		string(values, "sms", "sms");
		integer(values, "sms.max_count", json_object_get_int(get(args, "max_count")));
		integer(values, "sms.max_sent_count",
			json_object_get_int(get(args, "max_sent_count")));
		string(values, "sms.direct_receive",
		       json_object_get_boolean(get(args, "direct_receive")) ? "1" : "0");
		int rc = cell_config_commit(values, NULL);
		json_object_put(values);
		if (rc < 0)
			goto fail;
	} else {
		const char *folder =
		    !strcmp(operation, "sms_mark_read") ? "inbox" : str(args, "folder");
		J *rows = get(data, folder),
		  *visible = !strcmp(folder, "inbox") ? sms_inbox(rows) : jclone(rows);
		if (!visible)
			goto fail;
		J *selected = NULL;
		for (size_t i = 0; i < json_object_array_length(visible); i++) {
			J *row = json_object_array_get_idx(visible, i);
			if (text_equal(str(row, "id"), str(args, "id"))) {
				selected = row;
				break;
			}
		}
		if (!selected) {
			json_object_put(visible);
			errorf(CELL_INVALID, "SMS history selection no longer exists");
			goto fail;
		}
		J *ids = get(selected, "part_ids") ? json_object_get(get(selected, "part_ids"))
						   : json_object_new_array();
		if (!get(selected, "part_ids"))
			json_object_array_add(ids, json_object_get(get(selected, "id")));
		for (size_t i = json_object_array_length(rows); i > 0; i--) {
			J *row = json_object_array_get_idx(rows, i - 1);
			if (id_in(ids, str(row, "id"))) {
				if (!strcmp(operation, "sms_mark_read"))
					boolean(row, "read", true);
				else
					json_object_array_del_idx(rows, i - 1, 1);
			}
		}
		json_object_put(ids);
		json_object_put(visible);
	}
	if (save(data) < 0)
		goto fail;
	json_object_put(data);
	return summary(c);
fail:
	json_object_put(data);
	return NULL;
}

int ims_initialize(struct controller *c)
{
	const char *commands[] = {"AT+CIREG=2", "AT+IMSEN=1"};
	J *rows = json_object_new_array();
	bool accepted = true;
	for (size_t i = 0; i < 2; i++) {
		char *raw = controller_at(c, commands[i], 15);
		if (!raw && cell_error_kind != CELL_MODEM && cell_error_kind != CELL_INVALID) {
			json_object_put(rows);
			return -1;
		}
		J *r = json_object_new_object();
		string(r, "command", commands[i]);
		boolean(r, "accepted", raw != NULL);
		if (!raw) {
			accepted = false;
			string(r, "error", cell_error);
		}
		free(raw);
		json_object_array_add(rows, r);
	}
	J *result = json_object_new_object();
	boolean(result, "accepted", accepted);
	json_object_object_add(result, "commands", rows);
	json_object_object_add(c->status, "ims_initialization", result);
	return 0;
}

int ims_refresh(struct controller *c)
{
	J *row = controller_query(c, "AT+CIREG?", 5, "+CIREG:");
	if (!row && (cell_error_kind != CELL_MODEM && cell_error_kind != CELL_INVALID))
		return -1;
	json_object_object_add(c->status, "ims", row);
	return 0;
}

static J *delete_inbox_message(struct controller *c, J *args)
{
	J *data = load();
	if (!data)
		return NULL;
	J *visible = sms_inbox(get(data, "inbox"));
	json_object_put(data);
	if (!visible)
		return NULL;
	J *ids = NULL;
	for (size_t i = 0; i < json_object_array_length(visible); i++) {
		J *row = json_object_array_get_idx(visible, i);
		if (text_equal(str(row, "id"), str(args, "id"))) {
			ids = get(row, "part_ids") ? jclone(get(row, "part_ids"))
						   : json_object_new_array();
			if (!get(row, "part_ids"))
				json_object_array_add(ids, json_object_get(get(row, "id")));
			break;
		}
	}
	json_object_put(visible);
	if (!ids) {
		errorf(CELL_INVALID, "SMS history selection no longer exists");
		return NULL;
	}
	J *snapshot = sms_refresh(c);
	if (!snapshot) {
		json_object_put(ids);
		return NULL;
	}
	J *rows = get(snapshot, "messages");
	bool ok = true;
	for (size_t i = 0; i < json_object_array_length(rows); i++) {
		J *row = json_object_array_get_idx(rows, i);
		if (!id_in(ids, str(row, "pdu_sha256")))
			continue;
		J *selection = json_object_new_object();
		json_object_object_add(selection, "index", json_object_get(get(row, "index")));
		string(selection, "pdu_sha256", str(row, "pdu_sha256"));
		J *deleted = sms_execute(c, "sms_delete", selection);
		json_object_put(selection);
		if (!deleted) {
			ok = false;
			break;
		}
		json_object_put(deleted);
	}
	json_object_put(snapshot);
	json_object_put(ids);
	return ok ? history_execute(c, "sms_delete_history", args) : NULL;
}

J *sms_execute(struct controller *c, const char *operation, J *args)
{
	if (transport_admit(&c->transport) < 0)
		return NULL;
	if (!strcmp(operation, "sms_history") || !strcmp(operation, "sms_settings") ||
	    !strcmp(operation, "sms_mark_read") || !strcmp(operation, "sms_delete_history"))
		return history_execute(c, operation, args);
	if (!strcmp(operation, "sms_delete_batch")) {
		J *items = get(args, "items"), *out = json_object_new_object(),
		  *rows = json_object_new_array();
		bool complete = true;
		for (size_t i = 0; i < json_object_array_length(items); i++) {
			J *item = json_object_array_get_idx(items, i),
			  *row = json_object_new_object();
			J *result = text_equal(str(item, "folder"), "inbox")
					? delete_inbox_message(c, item)
					: sms_execute(c,
						      get(item, "index") ? "sms_delete"
									 : "sms_delete_history",
						      item);
			json_object_object_add(row, "selection", jclone(item));
			boolean(row, "deleted", result != NULL);
			if (!result) {
				complete = false;
				string(row, "error", cell_error);
			}
			json_object_put(result);
			json_object_array_add(rows, row);
			if (c->transport.poisoned) {
				complete = false;
				break;
			}
		}
		boolean(out, "complete", complete);
		json_object_object_add(out, "items", rows);
		return out;
	}
	if (!strcmp(operation, "sms_refresh"))
		return sms_refresh(c);
	if (!strcmp(operation, "sms_send")) {
		if (sent_start(c, args) < 0) {
			c->sms_active_request[0] = 0;
			return NULL;
		}
		J *replies = json_object_new_array(), *parts = NULL;
		char *raw = controller_at(c, "AT+CMGF=0", 5);
		if (!raw)
			goto send_fail;
		free(raw);
		char random[3];
		if (random_hex(random, 1) < 0)
			goto send_fail;
		parts = sms_encode(str(args, "number"), str(args, "text"),
				   (unsigned)strtoul(random, NULL, 16));
		if (!parts)
			goto send_fail;
		double deadline = mono() + 200;
		for (size_t i = 0; i < json_object_array_length(parts); i++) {
			double remaining = deadline - mono();
			if (remaining <= 0) {
				errorf(CELL_UNCERTAIN, "Multipart SMS deadline reached; confirmed "
						       "parts recorded, no replay");
				goto send_fail;
			}
			J *part = json_object_array_get_idx(parts, i);
			char command[48];
			snprintf(command, sizeof(command), "AT+CMGS=%d",
				 json_object_get_int(get(part, "tpdu_length")));
			raw = transport_command(&c->transport, command,
						remaining < 138 ? remaining : 138, str(part, "pdu"),
						false);
			if (!raw)
				goto send_fail;
			J *row = at_one(raw, "+CMGS:");
			free(raw);
			int64_t reference;
			if (!row ||
			    !integer_text(json_object_get_string(json_object_array_get_idx(row, 0)),
					  0, 65535, &reference)) {
				json_object_put(row);
				transport_poison(&c->transport,
						 "SMS submission reference is invalid");
				goto send_fail;
			}
			json_object_put(row);
			J *reply = json_object_new_object();
			integer(reply, "part", i + 1);
			integer(reply, "total", json_object_array_length(parts));
			integer(reply, "reference", reference);
			json_object_array_add(replies, reply);
			J *update = json_object_new_object();
			json_object_object_add(update, "parts", json_object_get(replies));
			int rc = sent_update(c, str(args, "request_id"), update);
			json_object_put(update);
			if (rc < 0)
				goto send_fail;
			J *progress = json_object_new_object();
			string(progress, "request_id", str(args, "request_id"));
			json_object_object_add(progress, "confirmed_parts",
					       json_object_get(replies));
			boolean(progress, "delivery_confirmed", false);
			rc = atomic_json(CELL_RUN "/sms-send-progress.json", progress, false);
			json_object_put(progress);
			if (rc < 0)
				goto send_fail;
		}
		J *update = json_object_new_object();
		string(update, "state", "submitted");
		int rc = sent_update(c, str(args, "request_id"), update);
		json_object_put(update);
		if (rc < 0)
			goto send_fail;
		c->sms_active_request[0] = 0;
		J *result = json_object_new_object();
		boolean(result, "accepted", true);
		json_object_object_add(result, "parts", replies);
		boolean(result, "modem_submission_confirmed", true);
		boolean(result, "delivery_confirmed", false);
		json_object_put(parts);
		return result;
	send_fail:;
		char saved[512];
		snprintf(saved, sizeof(saved), "%s", cell_error);
		enum cell_error_kind kind = cell_error_kind;
		J *failed = json_object_new_object();
		string(failed, "state",
		       c->transport.poisoned || json_object_array_length(replies) ? "uncertain"
										  : "failed");
		string(failed, "error", saved);
		json_object_object_add(failed, "parts", json_object_get(replies));
		sent_update(c, str(args, "request_id"), failed);
		json_object_put(failed);
		c->sms_active_request[0] = 0;
		json_object_put(replies);
		json_object_put(parts);
		errorf(kind, "%s", saved);
		return NULL;
	}
	if (!strcmp(operation, "sms_delete")) {
		J *value = sms_refresh(c);
		if (!value)
			return NULL;
		J *rows = get(value, "messages");
		bool match = false;
		int index = json_object_get_int(get(args, "index"));
		for (size_t i = 0; i < json_object_array_length(rows); i++) {
			J *r = json_object_array_get_idx(rows, i);
			if (json_object_get_int(get(r, "index")) == index &&
			    text_equal(str(r, "pdu_sha256"), str(args, "pdu_sha256")))
				match = true;
		}
		json_object_put(value);
		if (!match) {
			errorf(CELL_INVALID, "SMS selection changed; refresh before deleting");
			return NULL;
		}
		char command[48];
		snprintf(command, sizeof(command), "AT+CMGD=%d", index);
		char *raw = controller_at(c, command, 5);
		if (!raw)
			return NULL;
		free(raw);
		value = sms_refresh(c);
		if (!value)
			return NULL;
		rows = get(value, "messages");
		match = false;
		for (size_t i = 0; i < json_object_array_length(rows); i++) {
			J *r = json_object_array_get_idx(rows, i);
			if (json_object_get_int(get(r, "index")) == index &&
			    text_equal(str(r, "pdu_sha256"), str(args, "pdu_sha256")))
				match = true;
		}
		json_object_put(value);
		if (match) {
			errorf(CELL_MODEM, "SMS deletion readback failed");
			return NULL;
		}
		J *result = json_object_new_object();
		integer(result, "deleted_index", index);
		return result;
	}
	errorf(CELL_INVALID, "Unknown SMS operation");
	return NULL;
}
