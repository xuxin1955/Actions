/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SY108_MISC_STATE_H
#define SY108_MISC_STATE_H
#include <stddef.h>
#include <stdint.h>
#define UDX_MISC_ENV_BYTES 0x100000u
/* Installed native-loader format limits, not required UBI volume sizes. */
#define UDX_BOOT_VOLUME_BYTES (133u * 126976u)
#define UDX_RECOVERY_VOLUME_BYTES (149u * 126976u)
#define UDX_BOOT_STATE_KEY "udx_boot_state"

enum udx_upgrade_state {
	UDX_UPGRADE_INVALID,
	UDX_UPGRADE_READY,
	UDX_UPGRADE_PENDING,
	UDX_UPGRADE_RECOVERY,
	UDX_UPGRADE_TRIAL
};

enum udx_boot_choice { UDX_BOOT_CHOICE_RECOVERY = 1, UDX_BOOT_CHOICE_OPENWRT = 2 };

uint32_t udx_crc32(const void *, size_t);
enum udx_upgrade_state udx_misc_state(const void *, size_t, int read_error);
#endif
