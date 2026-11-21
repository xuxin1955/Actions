/* SPDX-License-Identifier: GPL-2.0-only */
#include "frequency.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>
#ifndef DEVICE_ROOT
#define DEVICE_ROOT ""
#endif
#define CPU DEVICE_ROOT "/sys/devices/system/cpu/cpufreq/policy0/"
#define DDR DEVICE_ROOT "/sys/class/devfreq/scene-frequency/"
#define LOCK DEVICE_ROOT "/run/udx-frequency.lock"
static const char *keys_list[] = {"cpu_governor", "cpu_min_khz", "cpu_max_khz"};

static char *read_value(const char *path)
{
	char *s = read_text(path, 4096, true);
	if (s) {
		size_t n = strlen(s);
		while (n && (s[n - 1] == '\n' || s[n - 1] == ' ' || s[n - 1] == '\t'))
			s[--n] = 0;
	}
	return s;
}

static void number(J *row, const char *key, const char *path)
{
	char *s = read_value(path);
	int64_t value;
	if (s && integer_text(s, 0, INT64_MAX, &value))
		integer(row, key, value);
	else
		json_object_object_add(row, key, NULL);
	free(s);
}

static void value(J *row, const char *key, const char *path)
{
	char *s = read_value(path);
	string(row, key, s);
	free(s);
}

static void choices(J *row, const char *key, const char *path, bool numeric)
{
	char *s = read_value(path), *save = NULL;
	J *list = json_object_new_array();
	for (char *p = s ? strtok_r(s, " \t\n", &save) : NULL; p;
	     p = strtok_r(NULL, " \t\n", &save)) {
		int64_t n;
		if (numeric && integer_text(p, 1, INT64_MAX, &n))
			json_object_array_add(list, json_object_new_int64(n));
		else if (!numeric)
			json_object_array_add(list, json_object_new_string(p));
	}
	free(s);
	json_object_object_add(row, key, list);
}

J *frequency_status(void)
{
	J *out = json_object_new_object(), *cpu = json_object_new_object();
	J *ddr = json_object_new_object();
	value(cpu, "governor", CPU "scaling_governor");
	number(cpu, "min_khz", CPU "scaling_min_freq");
	number(cpu, "max_khz", CPU "scaling_max_freq");
	number(cpu, "current_khz", CPU "scaling_cur_freq");
	choices(cpu, "governors", CPU "scaling_available_governors", false);
	choices(cpu, "frequencies_khz", CPU "scaling_available_frequencies", true);
	number(ddr, "current_hz", DDR "cur_freq");
	choices(ddr, "frequencies_hz", DDR "available_frequencies", true);
	boolean(ddr, "available", get(ddr, "current_hz") != NULL);
	json_object_object_add(out, "cpu", cpu);
	json_object_object_add(out, "ddr", ddr);
	boolean(out, "available",
		get(cpu, "min_khz") && get(cpu, "max_khz") && str(cpu, "governor"));
	return out;
}

static bool contains(J *list, J *wanted)
{
	for (size_t i = 0; i < json_object_array_length(list); i++)
		if (json_object_equal(json_object_array_get_idx(list, i), wanted))
			return true;
	return false;
}

static int set(const char *path, const char *text)
{
	int fd = open(path, O_WRONLY | O_TRUNC | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return errorf(CELL_SYSTEM, "Open frequency setting: %s", strerror(errno));
	size_t size = strlen(text);
	ssize_t n = write(fd, text, size);
	int saved = errno, closed = close(fd);
	return n == (ssize_t)size && !closed
		   ? 0
		   : errorf(CELL_SYSTEM, "Write frequency setting: %s", strerror(saved));
}

static int set_number(const char *path, int64_t n)
{
	char text[32];
	snprintf(text, sizeof(text), "%" PRId64 "\n", n);
	return set(path, text);
}

static int cpu_set(const char *governor, int64_t minimum, int64_t maximum, int64_t old_max)
{
	if (minimum > old_max) {
		if (set_number(CPU "scaling_max_freq", maximum) ||
		    set_number(CPU "scaling_min_freq", minimum))
			return -1;
	} else if (set_number(CPU "scaling_min_freq", minimum) ||
		   set_number(CPU "scaling_max_freq", maximum))
		return -1;
	return set(CPU "scaling_governor", governor);
}

static bool matches(J *current, const char *governor, int64_t minimum, int64_t maximum)
{
	J *cpu = get(current, "cpu");
	return text_equal(str(cpu, "governor"), governor) &&
	       json_object_get_int64(get(cpu, "min_khz")) == minimum &&
	       json_object_get_int64(get(cpu, "max_khz")) == maximum;
}

static int save(J *args)
{
	char batch[1024];
	int n = snprintf(batch, sizeof(batch),
			 "set udx-hardware.clocks=frequency\nset udx-hardware.clocks.enabled='1'\n"
			 "set udx-hardware.clocks.cpu_governor='%s'\n"
			 "set udx-hardware.clocks.cpu_min_khz='%" PRId64 "'\n"
			 "set udx-hardware.clocks.cpu_max_khz='%" PRId64 "'\n"
			 "commit udx-hardware\n",
			 str(args, "cpu_governor"), json_object_get_int64(get(args, "cpu_min_khz")),
			 json_object_get_int64(get(args, "cpu_max_khz")));
	if (n < 0 || n >= (int)sizeof(batch))
		return errorf(CELL_INVALID, "Frequency configuration too large");
	const char *argv[] = {"/sbin/uci", "-q", "batch", NULL};
	char *output = NULL;
	int rc = command_run(argv, batch, 8000, 65536, &output);
	free(output);
	if (rc) {
		const char *undo[] = {"/sbin/uci", "-q", "revert", "udx-hardware", NULL};
		output = NULL;
		(void)command_run(undo, NULL, 8000, 65536, &output);
		free(output);
		return errorf(CELL_SYSTEM, "Frequency configuration could not be saved");
	}
	return 0;
}

J *frequency_apply(J *args, bool persist)
{
	int lock = open(LOCK, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB)) {
		if (lock >= 0)
			close(lock);
		errorf(CELL_SYSTEM, "Frequency settings are busy");
		return NULL;
	}
	J *before = frequency_status(), *cpu = get(before, "cpu");
	J *out = NULL, *after = NULL;
	int64_t minimum = json_object_get_int64(get(args, "cpu_min_khz"));
	int64_t maximum = json_object_get_int64(get(args, "cpu_max_khz"));
	const char *cpu_gov = str(args, "cpu_governor");
	if (!keys(args, keys_list, 3) || json_object_object_length(args) != 3 ||
	    !json_object_get_boolean(get(before, "available")) ||
	    !json_object_is_type(get(args, "cpu_min_khz"), json_type_int) ||
	    !json_object_is_type(get(args, "cpu_max_khz"), json_type_int) ||
	    !contains(get(cpu, "governors"), get(args, "cpu_governor")) ||
	    !contains(get(cpu, "frequencies_khz"), get(args, "cpu_min_khz")) ||
	    !contains(get(cpu, "frequencies_khz"), get(args, "cpu_max_khz")) || minimum > maximum) {
		errorf(CELL_INVALID, "Choose a supported CPU governor and frequencies; minimum "
				     "must not exceed maximum");
		goto done;
	}
	int rc = cpu_set(cpu_gov, minimum, maximum, json_object_get_int64(get(cpu, "max_khz")));
	after = frequency_status();
	if (!rc && !matches(after, cpu_gov, minimum, maximum))
		rc = errorf(CELL_SYSTEM, "Frequency request readback mismatch");
	if (!rc && persist)
		rc = save(args);
	if (!rc) {
		out = after;
		after = NULL;
		boolean(out, "accepted", true);
		boolean(out, "readback_verified", true);
		boolean(out, "persisted", persist);
	} else {
		char error[512];
		snprintf(error, sizeof(error), "%s", cell_error);
		int undo_cpu =
		    cpu_set(str(cpu, "governor"), json_object_get_int64(get(cpu, "min_khz")),
			    json_object_get_int64(get(cpu, "max_khz")),
			    json_object_get_int64(get(get(after, "cpu"), "max_khz")));
		out = frequency_status();
		boolean(out, "accepted", false);
		string(out, "error", error);
		boolean(out, "rollback_verified",
			!undo_cpu && matches(out, str(cpu, "governor"),
					     json_object_get_int64(get(cpu, "min_khz")),
					     json_object_get_int64(get(cpu, "max_khz"))));
	}
done:
	json_object_put(before);
	json_object_put(after);
	close(lock);
	return out;
}

static char *config(const char *key)
{
	char name[128];
	snprintf(name, sizeof(name), "udx-hardware.clocks.%s", key);
	const char *argv[] = {"/sbin/uci", "-q", "get", name, NULL};
	char *out = NULL;
	if (command_run(argv, NULL, 3000, 4096, &out)) {
		free(out);
		return NULL;
	}
	if (out)
		out[strcspn(out, "\r\n")] = 0;
	return out;
}

int frequency_restore(void)
{
	char *enabled = config("enabled");
	bool run = text_equal(enabled, "1");
	free(enabled);
	if (!run)
		return 0;
	J *args = json_object_new_object();
	for (size_t i = 0; i < 3; i++) {
		char *s = config(keys_list[i]);
		int64_t n;
		if (i == 0)
			string(args, keys_list[i], s);
		else if (s && integer_text(s, 0, INT64_MAX, &n))
			integer(args, keys_list[i], n);
		free(s);
	}
	J *out = frequency_apply(args, false);
	json_object_put(args);
	bool ok = out && json_object_get_boolean(get(out, "accepted"));
	if (!ok)
		fprintf(stderr, "udx-frequency: %s\n", out ? str(out, "error") : cell_error);
	json_object_put(out);
	return ok ? 0 : 1;
}
