/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef UDX_MODEM_CMDLINE_H
#define UDX_MODEM_CMDLINE_H
#include <stddef.h>
#include <stdint.h>

#define MCC_AP_MAX_BYTES 4095u
#define MCC_LOG_CHANNEL_COUNT 3u

enum mcc_result {
	MCC_OK = 0,
	MCC_E_ARGUMENT = -1,
	MCC_E_INPUT = -2,
	MCC_E_MODEM_BOUNDARY = -3,
	MCC_E_CAPACITY = -4
};

struct mcc_log_tuple {
	uint32_t dst;
	uint32_t channel;
	uint32_t smem_cp_address;
};

/* Pure NR command-line construction. No file access, source discovery, default
 * tuple values or claims that caller-supplied metadata is trusted.
 *
 * Caller must supply current verified tuples in this exact order:
 *   slog_lte, slog_nr_phy, slog_v3_phy.
 * AP code 0x405b94 obtains them via 0x4055a8 from the public platform spool
 * metadata. The BSP spool.c base_addr_show identifies the first three values as
 * dst, channel and CP-visible sblock shared-memory address; that source is a
 * reference interpretation, not proof of the stock binary's kernel identity.
 *
 * ap_bytes excludes a final NUL and must be 1..4095 with no embedded NUL.
 * Preserve stock 0x402620 substring semantics: first occurrence of "modem="
 * discards everything through the next ASCII space. If no such space exists,
 * reject instead of reproducing its invalid-pointer behavior. If no occurrence
 * exists, keep all AP bytes. Other bytes, including whitespace, are preserved.
 * Append three exact " log_chan=sblock,%u,%u,%08X" entries and one final NUL.
 * out_bytes includes that final NUL. Capacity must be the actual target limit.
 * Failures return an empty output (if capacity is nonzero) and out_bytes=0;
 * partial appends never count as a command line. Input/output overlap is safe.
 */
int mcc_build_nr(const char *ap, size_t ap_bytes,
		 const struct mcc_log_tuple tuples[MCC_LOG_CHANNEL_COUNT], char *out,
		 size_t capacity, size_t *out_bytes);

/* AP admission policy, deliberately stricter than stock 0x403fb0's first
 * substring: exactly one complete modem= token, ASCII whitespace delimiters,
 * exact shutdown value. Duplicate tokens are invalid; absence/other distinct.
 * It is never authentication or proof of exclusive ownership/physical halt. */
enum mcc_boot_hint { MCC_BOOT_ABSENT = 0, MCC_BOOT_SHUTDOWN = 1, MCC_BOOT_OTHER = 2 };

int mcc_boot_hint_parse(const char *, size_t, enum mcc_boot_hint *);
#endif
