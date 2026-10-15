/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NNNIX_FMT_H
#define NNNIX_FMT_H
#include <stdarg.h>
#include <stddef.h>

/* printf subset: %d %i %u %x %X %p %s %c %%, flags '-' '0', width (or '*'), 'l' accepted.
 * Always NUL-terminates when size > 0; returns the length that would have been written. */
int nn_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int nn_snprintf(char *buf, size_t size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

#endif
