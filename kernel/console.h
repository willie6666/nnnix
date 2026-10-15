/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NNNIX_CONSOLE_H
#define NNNIX_CONSOLE_H
#include <stddef.h>
#include <stdint.h>

/* tty0: the onboard LCD for output and the CH9350 keyboard for input.
 * console_poll (kernel daemon) keeps an nsh running on it, starting a new one
 * after each logout. */
void console_init(void);
void console_poll(void);
void console_input(const uint8_t *bytes, int n);    /* keyboard bytes (any task) */
size_t console_snapshot(char *buf, size_t size);    /* screen text, for tests */

#endif
