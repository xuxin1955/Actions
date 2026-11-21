/* SPDX-License-Identifier: GPL-2.0-only */
/* Typed rpcd adapter; never opens an AT
 * device. */
#include "common.h"
#include "frequency.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <libubus.h>
#include <libubox/blobmsg.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#ifndef DEVICE_ROOT
#define DEVICE_ROOT ""
#endif
static const char *method_description =
    "{\"frequency_status\":{},\"frequency_apply\":{\"cpu_governor\":\"\",\"cpu_min_khz\":0,"
    "\"cpu_max_khz\":0},"
    "\"status\":{},\"usb\":{\"profile\":\"\",\"acm\":false}}";

static char *read_file(const char *name, size_t maximum)
{
	char path[PATH_MAX];
	if (snprintf(path, sizeof(path), "%s%s", DEVICE_ROOT, name) >= (int)sizeof(path))
		return NULL;
	return read_text(path, maximum, true);
}

static void text_file(J *o, const char *key, const char *name)
{
	char *value = read_file(name, 65536);
	string(o, key, value);
	free(value);
}

static bool file_exists(const char *name)
{
	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s%s", DEVICE_ROOT, name);
	struct stat st;
	return !stat(path, &st);
}

static void number_file(J *o, const char *key, const char *name, double divisor)
{
	char *s = read_file(name, 256);
	char *end = NULL;
	errno = 0;
	double value = s ? strtod(s, &end) : NAN;
	bool valid = s && *s && end && !*end && !errno && isfinite(value / divisor);
	if (valid)
		real(o, key, value / divisor);
	else
		json_object_object_add(o, key, NULL);
	free(s);
}

static J *unavailable(const char *error)
{
	J *out = json_object_new_object();
	boolean(out, "available", false);
	string(out, "error", error);
	return out;
}

static J *pairs(const char *raw)
{
	J *out = json_object_new_object();
	if (!raw)
		return out;
	char *copy = strdup(raw), *save = NULL;
	if (!copy)
		return out;
	for (char *line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		char *equals = strchr(line, '=');
		if (!equals || equals == line)
			continue;
		bool valid = true;
		for (char *p = line; p < equals; p++)
			if ((*p < 'a' || *p > 'z') && *p != '_')
				valid = false;
		if (valid) {
			*equals = 0;
			string(out, line, equals + 1);
		}
	}
	free(copy);
	return out;
}

static void runtime_reply(struct ubus_request *request, int type, struct blob_attr *message)
{
	J **result = request->priv;
	struct blob_attr *attr;
	size_t remaining;
	(void)type;
	if (!message)
		return;
	J *value = json_object_new_object();
	blobmsg_for_each_attr(attr, message, remaining)
	{
		const char *name = blobmsg_name(attr);
		J *item;
		switch (blobmsg_type(attr)) {
		case BLOBMSG_TYPE_STRING:
			item = json_object_new_string(blobmsg_get_string(attr));
			break;
		case BLOBMSG_TYPE_BOOL:
			item = json_object_new_boolean(blobmsg_get_bool(attr));
			break;
		case BLOBMSG_TYPE_INT32:
			item = json_object_new_int((int32_t)blobmsg_get_u32(attr));
			break;
		case BLOBMSG_TYPE_INT64:
			item = json_object_new_int64(blobmsg_get_u64(attr));
			break;
		default:
			continue;
		}
		json_object_object_add(value, name, item);
	}
	json_object_put(*result);
	*result = value;
}

static J *runtime_status(void)
{
	J *value = NULL;
	uint32_t id;
	alarm(3); /* Bound synchronous object lookup as well as the reply. */
	struct ubus_context *ctx = ubus_connect(NULL);
	if (!ctx) {
		alarm(0);
		return unavailable("Runtime ubus connection failed");
	}
	int ret = ubus_lookup_id(ctx, "udx.runtime", &id);
	if (!ret)
		ret = ubus_invoke(ctx, id, "status", NULL, runtime_reply, &value, 3000);
	ubus_free(ctx);
	alarm(0);
	if (ret || !value) {
		json_object_put(value);
		return unavailable(ret ? ubus_strerror(ret) : "Missing runtime response");
	}
	char *boot = read_file("/proc/sys/kernel/random/boot_id", 64);
	bool matches = text_equal(str(value, "boot_id"), boot);
	free(boot);
	if (!matches) {
		json_object_put(value);
		return unavailable("Runtime boot identity mismatch");
	}
	boolean(value, "available", true);
	return value;
}

static J *usb_status(void)
{
	char *raw = read_file("/run/udx-usb/applied", 65536);
	J *out = pairs(raw);
	free(raw);
	raw = read_file("/run/udx-usb/last-result", 65536);
	J *last = pairs(raw);
	free(raw);
	merge(out, last);
	json_object_put(last);
	text_file(out, "pending", "/run/udx-usb/pending");
	text_file(out, "udc", "/sys/kernel/config/usb_gadget/udx-usb/UDC");
	text_file(out, "state", "/sys/class/udc/29100000.usb/state");
	text_file(out, "speed", "/sys/class/udc/29100000.usb/current_speed");
	boolean(out, "available", get(out, "state") != NULL);
	J *profiles = json_object_new_array();
	json_object_array_add(profiles, json_object_new_string("ncm"));
	json_object_array_add(profiles, json_object_new_string("ncm-software"));
	json_object_array_add(profiles, json_object_new_string("ecm"));
	json_object_array_add(profiles, json_object_new_string("rndis"));
	json_object_object_add(out, "profiles", profiles);
	text_file(out, "offload_state", "/sys/bus/platform/devices/2b300000.pamu3/offload_state");
	return out;
}

static void child_path(char out[PATH_MAX], const char *base, const char *file)
{
	snprintf(out, PATH_MAX, "%s/%s", base, file);
}

static J *sysfs_array(const char *pattern, int kind)
{
	char rooted[PATH_MAX];
	snprintf(rooted, sizeof(rooted), "%s%s", DEVICE_ROOT, pattern);
	glob_t paths = {0};
	J *out = json_object_new_array();
	glob(rooted, 0, NULL, &paths);
	for (size_t i = 0; i < paths.gl_pathc; i++) {
		const char *path = paths.gl_pathv[i] + strlen(DEVICE_ROOT),
			   *name = strrchr(path, '/');
		name = name ? name + 1 : path;
		char sub[PATH_MAX];
		if (kind == 1 && strcmp(name, "scene-frequency"))
			continue;
		if (kind == 3 && !strcmp(name, "lo"))
			continue;
		J *row = json_object_new_object();
		if (kind == 0) {
			string(row, "id", name);
			child_path(sub, path, "type");
			text_file(row, "name", sub);
			child_path(sub, path, "temp");
			number_file(row, "temperature_c", sub, 1000);
		} else if (kind == 1) {
			string(row, "name", name);
			child_path(sub, path, "cur_freq");
			number_file(row, "current_hz", sub, 1);
			child_path(sub, path, "available_frequencies");
			text_file(row, "available_frequencies", sub);

		} else if (kind == 2) {
			string(row, "device", name);
			const char *text[] = {"name", "type"},
				   *numeric[] = {"data_bytes", "reserved_ebs"};
			for (size_t j = 0; j < 2; j++) {
				child_path(sub, path, text[j]);
				text_file(row, text[j], sub);
				child_path(sub, path, numeric[j]);
				number_file(row, numeric[j], sub, 1);
			}
		} else if (kind == 3) {
			string(row, "name", name);
			child_path(sub, path, "operstate");
			text_file(row, "operstate", sub);
			child_path(sub, path, "statistics/rx_bytes");
			number_file(row, "rx_bytes", sub, 1);
			child_path(sub, path, "statistics/tx_bytes");
			number_file(row, "tx_bytes", sub, 1);
		} else {
			string(row, "policy", name);
			child_path(sub, path, "scaling_cur_freq");
			number_file(row, "current_khz", sub, 1);
			child_path(sub, path, "scaling_governor");
			text_file(row, "governor", sub);
			child_path(sub, path, "scaling_min_freq");
			number_file(row, "min_khz", sub, 1);
			child_path(sub, path, "scaling_max_freq");
			number_file(row, "max_khz", sub, 1);
		}
		json_object_array_add(out, row);
	}
	globfree(&paths);
	return out;
}

static J *storage_status(void)
{
	J *out = json_object_new_array();
	char *raw = read_file("/proc/mounts", 65536), *save = NULL;
	if (!raw)
		return out;
	for (char *line = strtok_r(raw, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		char source[256], mount[PATH_MAX], type[64], options[512];
		if (sscanf(line, "%255s %4095s %63s %511s", source, mount, type, options) != 4 ||
		    (strcmp(mount, "/") && strcmp(mount, "/rom") && strcmp(mount, "/overlay") &&
		     strcmp(mount, "/tmp")))
			continue;
		J *row = json_object_new_object();
		string(row, "mount", mount);
		string(row, "source", source);
		string(row, "type", type);
		bool ro = false;
		char *next = NULL;
		for (char *opt = strtok_r(options, ",", &next); opt;
		     opt = strtok_r(NULL, ",", &next))
			if (!strcmp(opt, "ro"))
				ro = true;
		boolean(row, "readonly", ro);
		char path[PATH_MAX];
		int path_length = snprintf(path, sizeof(path), "%s%s", DEVICE_ROOT, mount);
		struct statvfs fs;
		if (path_length >= (int)sizeof(path) || statvfs(path, &fs)) {
			json_object_object_add(row, "total_bytes", NULL);
			json_object_object_add(row, "free_bytes", NULL);
		} else {
			integer(row, "total_bytes", fs.f_blocks * fs.f_frsize);
			integer(row, "free_bytes", fs.f_bavail * fs.f_frsize);
		}
		json_object_array_add(out, row);
	}
	free(raw);
	return out;
}

static J *status(void)
{
	J *out = json_object_new_object();
	real(out, "sample_uptime", mono());
	J *boot = json_object_new_object();
	text_file(boot, "policy", "/sys/firmware/devicetree/base/chosen/udx,boot-policy");
	text_file(boot, "target", "/sys/firmware/devicetree/base/chosen/udx,boot-target");
	text_file(boot, "root_marker", "/etc/udx-openwrt-root");
	json_object_object_add(out, "boot", boot);
	json_object_object_add(out, "runtime", runtime_status());
	json_object_object_add(out, "thermal", sysfs_array("/sys/class/thermal/thermal_zone*", 0));
	json_object_object_add(out, "ddr", sysfs_array("/sys/class/devfreq/*", 1));
	json_object_object_add(out, "cpu",
			       sysfs_array("/sys/devices/system/cpu/cpufreq/policy*", 4));
	json_object_object_add(out, "storage", storage_status());
	json_object_object_add(out, "ubi", sysfs_array("/sys/class/ubi/ubi[0-9]*_[0-9]*", 2));
	json_object_object_add(out, "usb", usb_status());
	J *upgrade = json_object_new_object();
	boolean(upgrade, "enabled", file_exists("/lib/upgrade/platform.sh"));
	json_object_object_add(out, "upgrade", upgrade);
	return out;
}

static J *rejected(const char *error, bool uncertain)
{
	J *out = json_object_new_object();
	boolean(out, "accepted", false);
	if (uncertain)
		boolean(out, "uncertain", true);
	string(out, "error", error);
	return out;
}

static bool usb_profile_supported(const char *profile)
{
	return text_equal(profile, "ncm") || text_equal(profile, "ncm-software") ||
	       text_equal(profile, "ecm") || text_equal(profile, "rndis");
}

static J *usb_request(J *arguments)
{
	const char *profile = str(arguments, "profile");
	J *acm = get(arguments, "acm");
	if (!usb_profile_supported(profile) ||
	    !json_object_is_type(acm, json_type_boolean)) {
		errorf(CELL_INVALID, "Invalid USB profile");
		return NULL;
	}
	const char *argv[] = {"/usr/sbin/udx-usbctl", "switch", profile,
			      json_object_get_boolean(acm) ? "acm" : "no-acm", NULL};
	char *output = NULL;
	int rc = command_run(argv, NULL, 8000, 65536, &output);
	if (rc < 0)
		return rejected(cell_error, cell_error_kind == CELL_UNCERTAIN);
	free(output);
	J *row = json_object_new_object();
	boolean(row, "accepted", true);
	string(row, "state", "queued");
	return row;
}

static J *dispatch(const char *method, J *args)
{
	J *methods = parse_json(method_description, strlen(method_description));
	J *schema = get(methods, method);
	bool valid = schema && json_object_is_type(args, json_type_object) &&
		     json_object_object_length(schema) == json_object_object_length(args);
	if (valid) {
		json_object_object_foreach(schema, key, value)
		{
			(void)value;
			J *entry;
			if (!json_object_object_get_ex(args, key, &entry))
				valid = false;
		}
	}
	json_object_put(methods);
	if (!valid) {
		errorf(CELL_INVALID, "Unknown method or fields");
		return NULL;
	}
	if (!strcmp(method, "frequency_status"))
		return frequency_status();
	if (!strcmp(method, "frequency_apply"))
		return frequency_apply(args, true);
	if (!strcmp(method, "status"))
		return status();
	if (!strcmp(method, "usb"))
		return usb_request(args);
	return NULL;
}

int main(int argc, char **argv)
{
	signal(SIGPIPE, SIG_IGN);
	if (argc == 2 && !strcmp(argv[1], "apply-frequency"))
		return frequency_restore();
	if (argc == 2 && !strcmp(argv[1], "list")) {
		puts(method_description);
		return 0;
	}
	if (argc != 3 || strcmp(argv[1], "call"))
		return 2;
	char input[8193];
	size_t n = fread(input, 1, sizeof(input), stdin);
	J *args = NULL, *result = NULL;
	if (n > 8192)
		errorf(CELL_INVALID, "Request too large");
	else if (ferror(stdin))
		errorf(CELL_SYSTEM, "Request read failed");
	else {
		args = parse_json(n ? input : "{}", n ? n : 2);
		if (args) {
			if (!json_object_is_type(args, json_type_object))
				errorf(CELL_INVALID, "Expected object");
			else {
				J *session = NULL;
				bool has =
				    json_object_object_get_ex(args, "ubus_rpc_session", &session);
				if (has && !hex_string(str(args, "ubus_rpc_session"), 32))
					errorf(CELL_INVALID, "Invalid RPC session metadata");
				else {
					if (has)
						json_object_object_del(args, "ubus_rpc_session");
					result = dispatch(argv[2], args);
				}
			}
		}
	}
	if (!result)
		result = rejected(cell_error, false);
	puts(json_object_to_json_string_ext(result, JSON_C_TO_STRING_PLAIN |
							JSON_C_TO_STRING_NOSLASHESCAPE));
	json_object_put(result);
	json_object_put(args);
	return 0;
}
