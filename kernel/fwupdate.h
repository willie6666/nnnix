/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NNNIX_KERNEL_FWUPDATE_H
#define NNNIX_KERNEL_FWUPDATE_H
#include "../lib/sys.h"

/* SYS_FWUPDATE (kernel/fwupdate.c). */
int fw_update(const char *path, int flags, struct nn_fwinfo *info);

#endif
