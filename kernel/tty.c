/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "tty.h"
#include "../arch/nuc140/arch.h"

volatile uint32_t tty_idle_logout_s;

static void out(struct tty *t, const char *s, size_t n)
{
    mutex_lock(&t->wlock);
    t->ops->write(t->ctx, s, n);
    mutex_unlock(&t->wlock);
}

static void echo(void *ctx, const char *s, size_t n) { out(ctx, s, n); }

void tty_init(struct tty *t, const struct tty_ops *ops, void *ctx)
{
    t->ops = ops;
    t->ctx = ctx;
    t->rpos = t->rlen = 0;
    t->ahead_len = t->reading = t->interrupt_pending = t->ahead_drop = 0;
    /* One byte is kept back so the line can be handed out with its '\n'. */
    ledit_init(&t->ed, t->line, TTY_LINE - 1, echo, t);
}

static int next_byte(struct tty *t, uint32_t timeout_ms)
{
    if (t->ahead_len) {
        int c = t->ahead[0];
        for (uint8_t i = 1; i < t->ahead_len; ++i) t->ahead[i - 1] = t->ahead[i];
        t->ahead_len--;
        return c;
    }
    return t->ops->getc(t->ctx, timeout_ms);
}

void tty_interrupt(struct tty *t) { t->interrupt_pending = 1; }

static int take_interrupt(struct tty *t)
{
    if (!t->interrupt_pending) return 0;
    t->interrupt_pending = 0;
    t->ahead_len = 0;              /* a cancelled partial command must not run later */
    t->ahead_drop = 0;
    t->rpos = t->rlen = 0;
    return 1;
}

int tty_interrupted(struct tty *t)
{
    if (t->reading) return 0;
    if (take_interrupt(t)) return 1;
    if (!t->ops->poll) return 0;
    int c;
    /* Keep looking for ^C when type-ahead is full. Bound the work even if a
     * peer streams input continuously; never execute a truncated command. */
    for (unsigned budget = TTY_LINE; budget && (c = t->ops->poll(t->ctx)) >= 0; --budget) {
        if (c == 0x03) {
            t->ahead_len = t->ahead_drop = 0;
            t->rpos = t->rlen = 0;
            return 1;
        }
        if (t->ahead_drop || t->ahead_len == sizeof t->ahead) {
            t->ahead_len = 0;
            t->ahead_drop = c != '\r' && c != '\n' && c != 0x04;
        } else t->ahead[t->ahead_len++] = (uint8_t)c;
    }
    if (c == TTY_HANGUP) {
        t->ahead_len = t->ahead_drop = 0;
        t->rpos = t->rlen = 0;
        return 1;                   /* unwind foreground work; next read observes EOF */
    }
    return 0;
}

static int read_line(struct tty *t, char *buf, size_t n);

int tty_read(struct tty *t, char *buf, size_t n)
{
    t->reading = 1;
    int r = read_line(t, buf, n);
    t->reading = 0;
    return r;
}

static int read_line(struct tty *t, char *buf, size_t n)
{
    uint32_t idle_since = jiffies;
    if (take_interrupt(t)) goto interrupted;
    while (t->rpos >= t->rlen) {
        /* Wait at most 1 s at a time, so a changed idle_logout applies to waiting readers too. */
        uint32_t limit = tty_idle_logout_s * 1000u, idle = jiffies - idle_since, wait = 1000u;
        if (limit && idle >= limit) {                 /* idle: end of file, the shell logs out */
            static const char msg[] = "\r\nidle timeout\r\n";
            out(t, msg, sizeof msg - 1);
            return 0;
        }
        if (limit && limit - idle < wait) wait = limit - idle;
        int c = next_byte(t, wait);
        if (take_interrupt(t)) {
            /* A byte arriving after SW_INT must reach the next prompt. */
            if (c >= 0 && c != 0x03) t->ahead[t->ahead_len++] = (uint8_t)c;
            goto interrupted;
        }
        if (c == TTY_TIMEOUT) continue;
        if (c < 0) return 0;                          /* hung up */
        idle_since = jiffies;
        if (t->ahead_drop && c != 0x03) {
            if (c == '\r' || c == '\n' || c == 0x04) t->ahead_drop = 0;
            continue;
        }
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
interrupted:
    t->rpos = t->rlen = 0;
    ledit_init(&t->ed, t->line, TTY_LINE - 1, echo, t);
    ledit_feed(&t->ed, 0x03);       /* reset partial lines/escape sequences and echo ^C */
    return -EINTR;
}

void tty_write(struct tty *t, const char *s, size_t n)
{
    size_t start = 0;
    mutex_lock(&t->wlock);                            /* the whole call, so lines stay together */
    for (size_t i = 0; i < n; ++i) {
        if (s[i] != '\n') continue;
        if (i > start) t->ops->write(t->ctx, s + start, i - start);
        t->ops->write(t->ctx, "\r\n", 2);
        start = i + 1;
    }
    if (n > start) t->ops->write(t->ctx, s + start, n - start);
    mutex_unlock(&t->wlock);
}
