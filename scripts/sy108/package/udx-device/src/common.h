/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef UDX_CELL_COMMON_H
#define UDX_CELL_COMMON_H
#define _GNU_SOURCE
#include <json-c/json.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef struct json_object J;

enum cell_error_kind { CELL_INVALID, CELL_SYSTEM, CELL_MODEM, CELL_UNCERTAIN };

extern _Thread_local char cell_error[512];
extern _Thread_local enum cell_error_kind cell_error_kind;
int errorf(enum cell_error_kind kind, const char *format, ...)
    __attribute__((format(printf, 2, 3)));
double mono(void);
void delay_ms(unsigned ms);
int random_hex(char *out, size_t bytes);
int sha256_hex(const void *data, size_t length, char out[65]);
bool hex_string(const char *value, size_t length);
bool text_equal(const char *a, const char *b);
bool integer_text(const char *text, int64_t minimum, int64_t maximum, int64_t *out);
J *get(J *o, const char *key);
const char *str(J *o, const char *key);
void string(J *o, const char *key, const char *value);
void integer(J *o, const char *key, int64_t value);
void boolean(J *o, const char *key, bool value);
void real(J *o, const char *key, double value);
J *jclone(J *o);
void merge(J *destination, J *source);
bool keys(J *o, const char *const *names, size_t count);
J *parse_json(const char *data, size_t length);
J *parse_json_document(const char *data, size_t length);
int sms_request_digest(const char *operation, J *args, char out[65]);
char *read_text(const char *path, size_t maximum, bool trim);
J *read_json(const char *path, size_t maximum);
int atomic_json(const char *path, J *value, bool durable);
int mkdir_private(const char *path);
int command_run(const char *const argv[], const char *input, unsigned timeout_ms, size_t maximum,
		char **output);
J *cell_config_sections(void);
int cell_config_commit(J *values, J *deletes);
J *cell_bus_call(const char *method, J *arguments, unsigned timeout_ms);
J *at_fields(const char *raw, const char *prefix);
J *at_one(const char *raw, const char *prefix);
bool at_complete(const char *raw);
J *sms_decode(const char *hex);
J *sms_encode(const char *number, const char *text, unsigned reference);
J *sms_list(const char *raw);
int sms_validate_number(const char *number);
#endif
