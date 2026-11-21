/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef CPNV_NV_UBI_LINUX_H
#define CPNV_NV_UBI_LINUX_H
#include "nv_ubi_backend.h"

struct nv_ubi_linux_context {
	void *owner;
	int (*check_io)(void *);
};
struct nv_ubi_ops nv_ubi_linux_ops(struct nv_ubi_linux_context *);
#endif
