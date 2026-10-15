/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "tty.h"

static void echo(void *ctx, const char *s, size_t n)
{
    struct tty *t = ctx;
    t->ops->write(t->ctx, s, n);
}

void tty_init(struct tty *t, const struct tty_ops *ops, void *ctx)
{
    t->ops = ops;
    t->ctx = ctx;
    t->rpos = t->rlen = 0;
    /* One byte is kept back so the line can be handed out with its '\n'. */
    ledit_init(&t->ed, t->line, TTY_LINE - 1, echo, t);
}

int tty_read(struct tty *t, char *buf, size_t n)
{
    while (t->rpos >= t->rlen) {
        int c = t->ops->getc(t->ctx);
        if (c < 0) return 0;                          /* hung up */
        switch (ledit_feed(&t->ed, (uint8_t)c)) {
        case LEDIT_LINE:
            t->line[t->ed.len] = '\n';
            t->rlen = (uint16_t)(t->ed.len + 1u);
            t->rpos = 0;
            break;
        case LEDIT_EOF:
            return 0;
        case LEDIT_INTR:
            return -EINTR;
        }
    }
    size_t k = 0;
    while (k < n && t->rpos < t->rlen) buf[k++] = t->line[t->rpos++];
    return (int)k;
}

void tty_write(struct tty *t, const char *s, size_t n)
{
    size_t start = 0;
    for (size_t i = 0; i < n; ++i) {
        if (s[i] != '\n') continue;
        if (i > start) t->ops->write(t->ctx, s + start, i - start);
        t->ops->write(t->ctx, "\r\n", 2);
        start = i + 1;
    }
    if (n > start) t->ops->write(t->ctx, s + start, n - start);
}
