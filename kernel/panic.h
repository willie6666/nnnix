/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NNNIX_PANIC_H
#define NNNIX_PANIC_H

/* Print, record the message for the next boot, reboot. */
void panic(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));

#endif
