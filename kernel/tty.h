/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NNNIX_TTY_H
#define NNNIX_TTY_H
#include <stddef.h>
#include <stdint.h>
#include "ledit.h"
#include "sync.h"

#define TTY_LINE 96
#define EINTR 4

/* A terminal: canonical line editing on top of a byte backend (telnet socket,
 * later the LCD + keyboard and the serial port). Used by one session task. */
#define TTY_TIMEOUT (-2)
struct tty_ops {
    /* Next input byte; -1 when hung up, TTY_TIMEOUT after timeout_ms (0: wait forever). */
    int  (*getc)(void *ctx, uint32_t timeout_ms);
    void (*write)(void *ctx, const char *s, size_t n);
    int  (*poll)(void *ctx);                           /* a byte if one is waiting, else -1 */
};

struct tty {
    const struct tty_ops *ops;
    void *ctx;
    struct ledit ed;
    char line[TTY_LINE];
    uint16_t rpos, rlen;             /* finished line not yet read */
    uint8_t ahead[16], ahead_len;    /* typed while a command ran; read before new input */
    volatile uint8_t reading;        /* a task is inside tty_read */
    /* A shell, its background jobs and pipeline stages all write here; the
     * backend's send (socket pointers, LCD cursor) must not interleave. */
    struct mutex wlock;
};

/* Idle logout (sysctl idle_logout): a terminal read with no input for this
 * many seconds returns end of file, so a waiting shell logs out. 0: never.
 * Readers check it every second, so a change also applies to them. */
extern volatile uint32_t tty_idle_logout_s;

void tty_init(struct tty *t, const struct tty_ops *ops, void *ctx);
int  tty_read(struct tty *t, char *buf, size_t n);    /* bytes; 0 at end of file; -EINTR on Ctrl-C */
void tty_write(struct tty *t, const char *s, size_t n);   /* "\n" becomes "\r\n" */
/* Non-blocking check for Ctrl-C while nobody reads the terminal: pending input
 * is moved to the type-ahead buffer; returns 1 (and drops the ^C) if one came. */
int  tty_interrupted(struct tty *t);

#endif
