/* SPDX-License-Identifier: GPL-2.0-only */
#include "modem_cmdline.h"
#include <string.h>

static int failed(char *out, size_t capacity, size_t *out_bytes, int error)
{
	if (out && capacity)
		out[0] = '\0';
	if (out_bytes)
		*out_bytes = 0;
	return error;
}

static size_t decimal(char *out, uint32_t value)
{
	char reverse[10];
	size_t used = 0, i;
	do {
		reverse[used++] = (char)('0' + value % 10);
		value /= 10;
	} while (value);
	for (i = 0; i < used; ++i)
		out[i] = reverse[used - 1 - i];
	return used;
}

int mcc_build_nr(const char *ap, size_t ap_bytes,
		 const struct mcc_log_tuple tuples[MCC_LOG_CHANNEL_COUNT], char *out,
		 size_t capacity, size_t *out_bytes)
{
	static const char key[] = "modem=";
	static const char prefix[] = " log_chan=sblock,";
	static const char hex[] = "0123456789ABCDEF";
	char appended[3 * (sizeof(prefix) - 1 + 10 + 1 + 10 + 1 + 8)];
	size_t offset = 0, used = 0, suffix, i, j;
	if (!ap || !tuples || !out || !out_bytes || !capacity)
		return failed(out, capacity, out_bytes, MCC_E_ARGUMENT);
	if (!ap_bytes || ap_bytes > MCC_AP_MAX_BYTES || memchr(ap, '\0', ap_bytes))
		return failed(out, capacity, out_bytes, MCC_E_INPUT);
	for (i = 0; i + sizeof(key) - 1 <= ap_bytes; ++i) {
		if (memcmp(ap + i, key, sizeof(key) - 1))
			continue;
		for (offset = i; offset < ap_bytes && ap[offset] != ' '; ++offset)
			;
		if (offset == ap_bytes)
			return failed(out, capacity, out_bytes, MCC_E_MODEM_BOUNDARY);
		++offset;
		break;
	}
	for (i = 0; i < MCC_LOG_CHANNEL_COUNT; ++i) {
		memcpy(appended + used, prefix, sizeof(prefix) - 1);
		used += sizeof(prefix) - 1;
		used += decimal(appended + used, tuples[i].dst);
		appended[used++] = ',';
		used += decimal(appended + used, tuples[i].channel);
		appended[used++] = ',';
		for (j = 0; j < 8; ++j)
			appended[used++] = hex[(tuples[i].smem_cp_address >> ((7 - j) * 4)) & 15];
	}
	suffix = ap_bytes - offset;
	if (suffix >= capacity || used > capacity - suffix - 1)
		return failed(out, capacity, out_bytes, MCC_E_CAPACITY);
	/* Everything was validated before output changes, so an in-place suffix
	 * move is safe and errors cannot leave a truncated generated command. */
	memmove(out, ap + offset, suffix);
	memcpy(out + suffix, appended, used);
	out[suffix + used] = '\0';
	*out_bytes = suffix + used + 1;
	return MCC_OK;
}

static int boot_space(unsigned char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

int mcc_boot_hint_parse(const char *ap, size_t n, enum mcc_boot_hint *out)
{
	if (out)
		*out = MCC_BOOT_ABSENT;
	if (!out || !ap || !n || n > MCC_AP_MAX_BYTES || memchr(ap, 0, n))
		return MCC_E_INPUT;
	enum mcc_boot_hint result = MCC_BOOT_ABSENT;
	unsigned found = 0;
	for (size_t at = 0; at < n;) {
		while (at < n && boot_space((unsigned char)ap[at]))
			++at;
		size_t end = at;
		while (end < n && !boot_space((unsigned char)ap[end]))
			++end;
		if (end - at >= 6 && !memcmp(ap + at, "modem=", 6)) {
			if (found++)
				return MCC_E_INPUT;
			result = end - at == 14 && !memcmp(ap + at + 6, "shutdown", 8)
				     ? MCC_BOOT_SHUTDOWN
				     : MCC_BOOT_OTHER;
		}
		at = end;
	}
	*out = result;
	return MCC_OK;
}
