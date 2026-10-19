/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ledit.h"

void ledit_init(struct ledit *e, char *buf, uint16_t cap,
                void (*out)(void *ctx, const char *s, size_t n), void *ctx)
{
    *e = (struct ledit){ .buf = buf, .cap = cap, .out = out, .ctx = ctx };
    if (cap) buf[0] = 0;
}

static void echo(struct ledit *e, const char *s, size_t n) { if (e->out) e->out(e->ctx, s, n); }

int ledit_feed(struct ledit *e, uint8_t c)
{
    if (e->done) { e->done = 0; e->len = 0; e->buf[0] = 0; }
    if (e->after_cr) {
        e->after_cr = 0;
        if (c == '\n' || c == 0) return LEDIT_MORE;   /* second half of CR LF / CR NUL */
    }
    if (c == 0x03) e->esc = 0;                       /* ^C also interrupts an unfinished escape */
    if (e->esc == 1) {                                /* after ESC */
        e->esc = c == '[' || c == 'O' ? 2 : 0;
        return LEDIT_MORE;
    }
    if (e->esc == 2) {                                /* CSI: parameters until a final byte */
        if (c >= 0x40 && c <= 0x7e) e->esc = 0;
        return LEDIT_MORE;
    }
    switch (c) {
    case '\r':
        e->after_cr = 1;
        /* fall through */
    case '\n':
        e->buf[e->len] = 0;
        echo(e, "\r\n", 2);
        e->done = 1;
        return LEDIT_LINE;
    case 0x08:
    case 0x7f:
        if (e->len) { e->len--; echo(e, "\b \b", 3); }
        return LEDIT_MORE;
    case 0x15:                                        /* Ctrl-U */
        while (e->len) { e->len--; echo(e, "\b \b", 3); }
        return LEDIT_MORE;
    case 0x03:                                        /* Ctrl-C */
        e->len = 0;
        e->buf[0] = 0;
        echo(e, "^C\r\n", 4);
        return LEDIT_INTR;
    case 0x04:                                        /* Ctrl-D */
        return e->len ? LEDIT_MORE : LEDIT_EOF;
    case 0x1b:
        e->esc = 1;
        return LEDIT_MORE;
    default:
        if (c < 0x20 || c > 0x7e) return LEDIT_MORE;
        if (e->len + 1u >= e->cap) { echo(e, "\a", 1); return LEDIT_MORE; }
        e->buf[e->len++] = (char)c;
        echo(e, (const char *)&c, 1);
        return LEDIT_MORE;
    }
}
