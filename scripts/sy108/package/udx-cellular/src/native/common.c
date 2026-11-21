/* SPDX-License-Identifier: GPL-2.0-only */
#include "common.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <linux/magic.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

_Thread_local char cell_error[512];
_Thread_local enum cell_error_kind cell_error_kind;

int errorf(enum cell_error_kind kind, const char *format, ...)
{
	va_list ap;
	va_start(ap, format);
	vsnprintf(cell_error, sizeof(cell_error), format, ap);
	va_end(ap);
	cell_error_kind = kind;
	return -1;
}

double mono(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

void delay_ms(unsigned ms)
{
	struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000};
	while (nanosleep(&t, &t) && errno == EINTR) {
	}
}

bool text_equal(const char *a, const char *b)
{
	return a && b && !strcmp(a, b);
}

bool hex_string(const char *s, size_t n)
{
	if (!s || strlen(s) != n)
		return false;
	for (size_t i = 0; i < n; i++)
		if (!strchr("0123456789abcdef", s[i]))
			return false;
	return true;
}

bool integer_text(const char *s, int64_t low, int64_t high, int64_t *out)
{
	if (!s || !*s || (*s != '-' && !isdigit((unsigned char)*s)))
		return false;
	errno = 0;
	char *end;
	long long v = strtoll(s, &end, 10);
	if (errno || *end || v < low || v > high)
		return false;
	if (out)
		*out = v;
	return true;
}

int random_hex(char *out, size_t n)
{
	if (n > 64)
		return errorf(CELL_INVALID, "Random identifier too long");
	unsigned char bytes[64];
	size_t used = 0;
	while (used < n) {
		ssize_t r = getrandom(bytes + used, n - used, 0);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			return errorf(CELL_SYSTEM, "getrandom: %s", strerror(errno));
		used += (size_t)r;
	}
	for (size_t i = 0; i < n; i++) {
		out[2 * i] = "0123456789abcdef"[bytes[i] >> 4];
		out[2 * i + 1] = "0123456789abcdef"[bytes[i] & 15];
	}
	out[2 * n] = 0;
	return 0;
}

J *get(J *o, const char *key)
{
	J *v = NULL;
	if (o && json_object_is_type(o, json_type_object))
		json_object_object_get_ex(o, key, &v);
	return v;
}

const char *str(J *o, const char *key)
{
	J *v = get(o, key);
	return json_object_is_type(v, json_type_string) ? json_object_get_string(v) : NULL;
}

void string(J *o, const char *key, const char *s)
{
	json_object_object_add(o, key, s ? json_object_new_string(s) : NULL);
}

void integer(J *o, const char *key, int64_t n)
{
	json_object_object_add(o, key, json_object_new_int64(n));
}

void boolean(J *o, const char *key, bool v)
{
	json_object_object_add(o, key, json_object_new_boolean(v));
}

void real(J *o, const char *key, double v)
{
	json_object_object_add(o, key, isfinite(v) ? json_object_new_double(v) : NULL);
}

J *jclone(J *o)
{
	J *v = NULL;
	if (o && json_object_deep_copy(o, &v, NULL))
		return NULL;
	return v;
}

void merge(J *dst, J *src)
{
	if (!src)
		return;
	json_object_object_foreach(src, key, value)
	{
		json_object_object_add(dst, key, json_object_get(value));
	}
}

bool keys(J *o, const char *const *names, size_t count)
{
	if (!json_object_is_type(o, json_type_object) ||
	    (size_t)json_object_object_length(o) != count)
		return false;
	for (size_t i = 0; i < count; i++) {
		J *v;
		if (!json_object_object_get_ex(o, names[i], &v))
			return false;
	}
	return true;
}

/* json-c owns syntax; validate values that cannot cross the C string/AT ABI. */
static bool json_values_valid(J *value, bool document)
{
	switch (json_object_get_type(value)) {
	case json_type_string:
		return document || strlen(json_object_get_string(value)) ==
				       (size_t)json_object_get_string_len(value);
	case json_type_double:
		return isfinite(json_object_get_double(value));
	case json_type_array:
		for (size_t i = 0; i < json_object_array_length(value); i++)
			if (!json_values_valid(json_object_array_get_idx(value, i), document))
				return false;
		break;
	case json_type_object:
		json_object_object_foreach(value, key, child)
		{
			(void)key;
			if (!json_values_valid(child, document))
				return false;
		}
		break;
	default:
		break;
	}
	return true;
}

static J *parse_document(const char *data, size_t n, bool document)
{
	if (!data || !n || n > 2097152 || memchr(data, 0, n)) {
		errorf(CELL_INVALID, "JSON size or encoding invalid");
		return NULL;
	}
	struct json_tokener *t = json_tokener_new_ex(33);
	if (!t) {
		errorf(CELL_SYSTEM, "JSON allocation failed");
		return NULL;
	}
	json_tokener_set_flags(t, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
	J *o = json_tokener_parse_ex(t, data, (int)n);
	size_t end = json_tokener_get_parse_end(t);
	if (json_tokener_get_error(t) == json_tokener_continue && end == n)
		o = json_tokener_parse_ex(t, " ", 1);
	while (end < n && strchr(" \t\r\n", data[end]))
		end++;
	if (json_tokener_get_error(t) != json_tokener_success || end != n ||
	    !json_values_valid(o, document)) {
		json_object_put(o);
		o = NULL;
		errorf(CELL_INVALID, "Invalid JSON value or trailing data");
	}
	json_tokener_free(t);
	return o;
}

J *parse_json(const char *data, size_t n)
{
	return parse_document(data, n, false);
}

/* Stored SMS text may contain U+0000. Keys and RPC/command inputs stay strict. */
J *parse_json_document(const char *data, size_t n)
{
	return parse_document(data, n, true);
}

char *read_text(const char *path, size_t max, bool trim)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return NULL;
	size_t capacity = max < 4096 ? max : 4096, n = 0;
	char *p = malloc(capacity + 1);
	if (!p) {
		close(fd);
		return NULL;
	}
	for (;;) {
		if (n == capacity) {
			char extra;
			ssize_t r;
			do
				r = read(fd, &extra, 1);
			while (r < 0 && errno == EINTR);
			if (!r)
				break;
			if (r < 0 || n == max) {
				int saved = r < 0 ? errno : EFBIG;
				free(p);
				close(fd);
				errno = saved;
				return NULL;
			}
			size_t next = capacity > max / 2 ? max : capacity * 2;
			if (next <= capacity)
				next = max;
			char *grown = realloc(p, next + 1);
			if (!grown) {
				free(p);
				close(fd);
				return NULL;
			}
			p = grown;
			capacity = next;
			p[n++] = extra;
			if (n == capacity)
				continue;
		}
		ssize_t r = read(fd, p + n, capacity - n);
		if (r < 0 && errno == EINTR)
			continue;
		if (r < 0) {
			int saved = errno;
			free(p);
			close(fd);
			errno = saved;
			return NULL;
		}
		if (!r)
			break;
		n += (size_t)r;
	}
	close(fd);
	if (trim) {
		while (n && (!p[n - 1] || isspace((unsigned char)p[n - 1])))
			n--;
		size_t start = 0;
		while (start < n && isspace((unsigned char)p[start]))
			start++;
		memmove(p, p + start, n - start);
		n -= start;
	}
	p[n] = 0;
	return p;
}

J *read_json(const char *path, size_t maximum)
{
	char *p = read_text(path, maximum, false);
	if (!p) {
		int saved = errno;
		errorf(CELL_SYSTEM, "Read %s: %s", path, strerror(saved));
		errno = saved;
		return NULL;
	}
	errno = 0;
	J *o = parse_json(p, strlen(p));
	free(p);
	if (!o)
		errno = EINVAL;
	return o;
}

int mkdir_private(const char *path)
{
	struct stat st;
	if (mkdir(path, 0700) && errno != EEXIST)
		return errorf(CELL_SYSTEM, "mkdir %s: %s", path, strerror(errno));
	if (lstat(path, &st) || !S_ISDIR(st.st_mode) || st.st_uid != geteuid())
		return errorf(CELL_SYSTEM, "Unexpected private directory %s", path);
	return 0;
}

static int write_all(int fd, const char *p, size_t n)
{
	while (n) {
		ssize_t r = write(fd, p, n);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			return -1;
		p += r;
		n -= (size_t)r;
	}
	return 0;
}

int atomic_json(const char *path, J *o, bool durable)
{
	char temp[PATH_MAX], parent[PATH_MAX];
	if (snprintf(temp, sizeof(temp), "%s.new", path) >= (int)sizeof(temp) ||
	    strlen(path) >= sizeof(parent))
		return errorf(CELL_INVALID, "Path too long");
	snprintf(parent, sizeof(parent), "%s", path);
	char *slash = strrchr(parent, '/');
	if (!slash)
		return errorf(CELL_INVALID, "Expected absolute file path");
	*slash = 0;
	int fd = open(temp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (fd < 0)
		return errorf(CELL_SYSTEM, "Open %s: %s", temp, strerror(errno));
	const char *s = json_object_to_json_string_ext(o, JSON_C_TO_STRING_PLAIN |
							      JSON_C_TO_STRING_NOSLASHESCAPE);
	int r = write_all(fd, s, strlen(s));
	if (!r && durable)
		r = fsync(fd);
	if (close(fd) && !r)
		r = -1;
	if (!r)
		r = rename(temp, path);
	if (!r && durable) {
		fd = open(parent, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		if (fd < 0)
			r = -1;
		else {
			r = fsync(fd);
			close(fd);
		}
	}
	if (r)
		return errorf(CELL_SYSTEM, "Persist %s: %s", path, strerror(errno));
	return 0;
}

int command_run(const char *const argv[], const char *input, unsigned ms, size_t max, char **out)
{
	int in[2] = {-1, -1}, output[2] = {-1, -1};
	if (out)
		*out = NULL;
	if (pipe2(in, O_CLOEXEC) || pipe2(output, O_CLOEXEC | O_NONBLOCK))
		goto pipe_fail;
	pid_t pid = fork();
	if (pid < 0)
		goto pipe_fail;
	if (!pid) {
		setpgid(0, 0);
		dup2(in[0], STDIN_FILENO);
		dup2(output[1], STDOUT_FILENO);
		int null = open("/dev/null", O_WRONLY);
		if (null >= 0)
			dup2(null, STDERR_FILENO);
		close(in[0]);
		close(in[1]);
		close(output[0]);
		close(output[1]);
		execv(argv[0], (char *const *)argv);
		_exit(127);
	}
	setpgid(pid, pid);
	close(in[0]);
	close(output[1]);
	fcntl(in[1], F_SETFL, O_NONBLOCK);
	char *buffer = malloc(max + 1);
	if (!buffer) {
		kill(-pid, SIGKILL);
		close(in[1]);
		close(output[0]);
		waitpid(pid, NULL, 0);
		return errorf(CELL_SYSTEM, "Out of memory");
	}
	size_t used = 0, sent = 0, size = input ? strlen(input) : 0;
	double end = mono() + ms / 1000.0;
	int status = 0;
	bool done = false, overflow = false;
	while (mono() < end) {
		if (in[1] >= 0) {
			ssize_t n = size > sent ? write(in[1], input + sent, size - sent) : 0;
			if (n > 0)
				sent += (size_t)n;
			if (sent == size || (n < 0 && errno != EAGAIN && errno != EINTR)) {
				close(in[1]);
				in[1] = -1;
			}
		}
		ssize_t n = read(output[0], buffer + used, max - used);
		if (n > 0)
			used += (size_t)n;
		if (used == max) {
			overflow = true;
			break;
		}
		if (waitpid(pid, &status, WNOHANG) == pid) {
			done = true;
			while ((n = read(output[0], buffer + used, max - used)) > 0) {
				used += (size_t)n;
				if (used == max) {
					overflow = true;
					break;
				}
			}
			break;
		}
		struct pollfd fds[2] = {{output[0], POLLIN, 0}, {in[1], POLLOUT, 0}};
		poll(fds, 2, 20);
	}
	if (in[1] >= 0)
		close(in[1]);
	close(output[0]);
	if (!done) {
		kill(-pid, SIGKILL);
		while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
		}
	}
	if (!done || overflow || sent != size || !WIFEXITED(status) || WEXITSTATUS(status)) {
		free(buffer);
		return errorf(!done ? CELL_UNCERTAIN : CELL_SYSTEM, "%s failed%s", argv[0],
			      !done	 ? ": timeout, outcome uncertain"
			      : overflow ? ": output limit"
					 : "");
	}
	buffer[used] = 0;
	if (out)
		*out = buffer;
	else
		free(buffer);
	return 0;
pipe_fail:
	for (size_t i = 0; i < 2; i++) {
		if (in[i] >= 0)
			close(in[i]);
		if (output[i] >= 0)
			close(output[i]);
	}
	return errorf(CELL_SYSTEM, "Create command pipes: %s", strerror(errno));
}
