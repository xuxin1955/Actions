/* SPDX-License-Identifier: GPL-2.0-only */
#include "cellular.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int sync_directory(void)
{
	int fd = open(CELL_STORE "/requests", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0)
		return errorf(CELL_SYSTEM, "Open receipt directory: %s", strerror(errno));
	int rc = fsync(fd);
	int saved = errno;
	close(fd);
	return rc < 0 ? errorf(CELL_SYSTEM, "Sync receipt directory: %s", strerror(saved)) : 0;
}

J *receipt_reserve(const char *operation, J *args, J *row, int *result)
{
	*result = -1;
	const char *id = str(args, "request_id");
	if (!hex_string(id, 32)) {
		errorf(CELL_INVALID, "Invalid request identifier");
		return NULL;
	}
	if (mkdir_private(CELL_STORE) < 0 || mkdir_private(CELL_STORE "/requests") < 0)
		return NULL;
	char digest[65];
	if (sms_request_digest(operation, args, digest))
		return NULL;
	char path[512];
	snprintf(path, sizeof(path), CELL_STORE "/requests/%s.json", id);
	int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (fd < 0) {
		if (errno != EEXIST) {
			errorf(CELL_SYSTEM, "Reserve SMS receipt: %s", strerror(errno));
			return NULL;
		}
		J *old = read_json(path, 65536);
		if (!old)
			return NULL;
		if (!text_equal(str(old, "request_digest"), digest)) {
			json_object_put(old);
			errorf(CELL_INVALID,
			       "Request identifier was reused with different content");
			return NULL;
		}
		*result = 1;
		return old;
	}
	J *value = jclone(row);
	string(value, "request_id", id);
	string(value, "request_digest", digest);
	const char *data = json_object_to_json_string_ext(
	    value, JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE);
	size_t length = strlen(data), sent = 0;
	while (sent < length) {
		ssize_t n = write(fd, data + sent, length - sent);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0) {
			errorf(CELL_SYSTEM, "Write SMS receipt: %s", strerror(errno));
			close(fd);
			json_object_put(value);
			return NULL;
		}
		sent += (size_t)n;
	}
	if (fsync(fd) < 0) {
		errorf(CELL_SYSTEM, "Sync SMS receipt: %s", strerror(errno));
		close(fd);
		json_object_put(value);
		return NULL;
	}
	close(fd);
	if (sync_directory() < 0) {
		json_object_put(value);
		return NULL;
	}
	merge(row, value);
	json_object_put(value);
	*result = 0;
	return NULL;
}

int receipt_finish(J *row)
{
	const char *id = str(row, "request_id");
	if (!id)
		return 0;
	if (!hex_string(id, 32))
		return errorf(CELL_INVALID, "Invalid receipt identifier");
	char path[512];
	snprintf(path, sizeof(path), CELL_STORE "/requests/%s.json", id);
	J *value = jclone(row);
	int rc = atomic_json(path, value, true);
	json_object_put(value);
	return rc;
}
