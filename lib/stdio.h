/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NNNIX_STDIO_H
#define NNNIX_STDIO_H

/* User-side output through the write system call (standard output, fd 1).
 * printf formats into a 128-byte buffer; longer output is truncated. */
int printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int fputs_fd(const char *s, int fd);

#endif
