/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NNNIX_KLOG_H
#define NNNIX_KLOG_H
#include <stddef.h>

/* Kernel log: "[  seconds.ms] message" lines to UART0, also kept in a ring for dmesg. */
void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void klog_write(const char *buf, size_t n);
size_t klog_read(char *buf, size_t size);    /* oldest-first copy of the ring */

#endif
