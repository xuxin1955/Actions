/* SPDX-License-Identifier: GPL-2.0-only */
#include "common.h"
#include <openssl/sha.h>
#include <string.h>

int sha256_hex(const void *p, size_t n, char out[65])
{
	unsigned char digest[SHA256_DIGEST_LENGTH];
	if (!SHA256(p, n, digest))
		return errorf(CELL_SYSTEM, "SHA-256 failed");
	for (size_t i = 0; i < 32; i++) {
		out[2 * i] = "0123456789abcdef"[digest[i] >> 4];
		out[2 * i + 1] = "0123456789abcdef"[digest[i] & 15];
	}
	out[64] = 0;
	return 0;
}

/* Fixed key order preserves existing SMS receipt digests. */
int sms_request_digest(const char *operation, J *args, char out[65])
{
	const char *keys[] = {"number", "request_id", "text"};
	J *fields = json_object_new_object();
	for (size_t i = 0; i < 3; i++) {
		J *value = get(args, keys[i]);
		if (!json_object_is_type(value, json_type_string)) {
			json_object_put(fields);
			return errorf(CELL_INVALID, "Invalid SMS receipt arguments");
		}
		json_object_object_add(fields, keys[i], json_object_get(value));
	}
	J *input = json_object_new_array();
	json_object_array_add(input, json_object_new_string(operation));
	json_object_array_add(input, fields);
	const char *encoded = json_object_to_json_string_ext(
	    input, JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE);
	int result = sha256_hex(encoded, strlen(encoded), out);
	json_object_put(input);
	return result;
}
