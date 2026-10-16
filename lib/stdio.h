/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NNNIX_STDIO_H
#define NNNIX_STDIO_H

/* User-side output through the write system call (standard output, fd 1).
 * printf streams through a 32-byte buffer, so output length is unlimited. */
#include <stdarg.h>
int printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int vprintf(const char *fmt, va_list ap);
int fputs_fd(const char *s, int fd);

#endif
