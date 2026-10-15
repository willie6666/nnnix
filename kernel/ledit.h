/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NNNIX_LEDIT_H
#define NNNIX_LEDIT_H
#include <stddef.h>
#include <stdint.h>

/* Canonical-mode line editor, independent of the terminal backend (host tested).
 * Handles printable ASCII, Backspace/DEL, Ctrl-U (kill line), Ctrl-C (interrupt),
 * Ctrl-D on an empty line (end of file), CR, LF and CR LF / CR NUL pairs, and
 * swallows ANSI escape sequences such as arrow keys. Echo goes to `out`. */
enum { LEDIT_MORE, LEDIT_LINE, LEDIT_EOF, LEDIT_INTR };

struct ledit {
    char *buf;
    uint16_t cap, len;
    uint8_t esc, after_cr, done;
    void (*out)(void *ctx, const char *s, size_t n);
    void *ctx;
};

void ledit_init(struct ledit *e, char *buf, uint16_t cap,
                void (*out)(void *ctx, const char *s, size_t n), void *ctx);
/* Feed one input byte. On LEDIT_LINE, buf holds len bytes plus a NUL; the next
 * feed starts a fresh line. */
int ledit_feed(struct ledit *e, uint8_t c);

#endif
