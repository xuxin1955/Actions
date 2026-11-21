/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef UDX_FREQUENCY_H
#define UDX_FREQUENCY_H
#include "common.h"
J *frequency_status(void);
J *frequency_apply(J *args, bool persist);
int frequency_restore(void);
#endif
