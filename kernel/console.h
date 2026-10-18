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
size_t console_snapshot(char *buf, size_t size);    /* screen text as shown, for tests */
/* The text size: 0 normal (21x8), 1 small (32x10); Ctrl+- and Ctrl+= on the
 * keyboard. And the scrollback (PageUp/PageDown, Shift+Up/Down): bytes of
 * history kept while the console shell runs. Set when >= 0; return the
 * setting, or a negative error: invalid, or (the scrollback) no memory for the
 * new ring, the old one kept. While the shell runs without a ring (there was
 * no memory when it started), the scrollback's is 0. */
int console_text_size(int size);
void console_frame(uint8_t out[1024]);   /* the text's pixels as on the LCD (/dev/lcd layout), for tests */
int console_history(int bytes);

/* Graphics (/dev/lcd): while claimed, text output still updates the terminal
 * but not the LCD; release redraws the text. One holder at a time. */
int  console_lcd_claim(void);         /* 0, or SYS_EBUSY */
void console_lcd_write(uint32_t off, const void *p, uint32_t n);   /* lcd_bytes */
void console_lcd_release(void);

#endif
