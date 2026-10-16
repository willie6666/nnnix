/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NNNIX_CONSOLE_H
#define NNNIX_CONSOLE_H
#include <stddef.h>
#include <stdint.h>

/* tty0: the onboard LCD for output and the CH9350 keyboard for input.
 * console_poll (kernel daemon) runs an nsh on it according to the mode:
 *   on   a shell always, a new one after each logout
 *   ask  a shell once Enter is pressed (the default: no RAM while unused)
 *   off  no shell; one that is running sees end of input and logs out */
enum { CONSOLE_OFF, CONSOLE_ASK, CONSOLE_ON };
int  console_mode(int mode);          /* set when mode >= 0; returns the mode, -1 if invalid */
void console_init(void);
void console_poll(void);
void console_input(const uint8_t *bytes, int n);    /* keyboard bytes (any task) */
size_t console_snapshot(char *buf, size_t size);    /* screen text, for tests */

/* Graphics (/dev/lcd): while claimed, text output still updates the terminal
 * but not the LCD; release redraws the text. One holder at a time. */
int  console_lcd_claim(void);         /* 0, or SYS_EBUSY */
void console_lcd_write(uint32_t off, const void *p, uint32_t n);   /* lcd_bytes */
void console_lcd_release(void);

#endif
