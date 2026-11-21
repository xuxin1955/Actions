/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef CPNV_IO_H
#define CPNV_IO_H
#include <stdbool.h>
#include <stddef.h>
bool cpnv_port_pending(int);
int cpnv_port_open(const char *, int);
int cpnv_private_read(const char *, void *, size_t, bool);
int cpnv_read(const char *, void *, size_t);
#endif
