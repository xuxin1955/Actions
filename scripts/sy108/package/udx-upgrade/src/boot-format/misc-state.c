/* SPDX-License-Identifier: GPL-2.0-only
 * Parse and validate the misc environment transaction field.
 */
#include "misc-state.h"

uint32_t udx_crc32(const void *data, size_t size)
{
	const unsigned char *p = data;
	uint32_t crc = ~0u;
	for (size_t i = 0; i < size; i++) {
		crc ^= p[i];
		for (unsigned bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^ (0xedb88320u & -(crc & 1u));
	}
	return ~crc;
}

static int equal(const unsigned char *p, size_t n, const char *s)
{
	size_t i = 0;
	while (s[i]) {
		if (i >= n || p[i] != (unsigned char)s[i])
			return 0;
		i++;
	}
	return i == n;
}

enum udx_upgrade_state udx_misc_state(const void *data, size_t size, int read_error)
{
	const unsigned char *p = data;
	if (read_error || !p || size != UDX_MISC_ENV_BYTES)
		return UDX_UPGRADE_INVALID;
	uint32_t expected =
	    (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
	if (udx_crc32(p + 4, size - 4) != expected)
		return UDX_UPGRADE_INVALID;
	enum udx_upgrade_state state = UDX_UPGRADE_INVALID;
	unsigned found = 0;
	size_t pos = 4;
	while (pos < size) {
		if (!p[pos])
			return pos > 4 && p[pos - 1] == 0 && found == 1 ? state
									: UDX_UPGRADE_INVALID;
		size_t end = pos, sep = pos;
		while (end < size && p[end])
			end++;
		if (end == size)
			return UDX_UPGRADE_INVALID;
		while (sep < end && p[sep] != '=')
			sep++;
		if (sep == pos || sep == end)
			return UDX_UPGRADE_INVALID;
		if (equal(p + pos, sep - pos, UDX_BOOT_STATE_KEY)) {
			if (found++)
				return UDX_UPGRADE_INVALID;
			const unsigned char *v = p + sep + 1;
			size_t n = end - sep - 1;
			if (equal(v, n, "ready"))
				state = UDX_UPGRADE_READY;
			else if (equal(v, n, "trial"))
				state = UDX_UPGRADE_TRIAL;
			else if (equal(v, n, "pending"))
				state = UDX_UPGRADE_PENDING;
			else if (equal(v, n, "recovery"))
				state = UDX_UPGRADE_RECOVERY;
			else
				return UDX_UPGRADE_INVALID;
		}
		pos = end + 1;
	}
	return UDX_UPGRADE_INVALID;
}
