/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NNNIX_TTY_H
#define NNNIX_TTY_H
#include <stddef.h>
#include <stdint.h>
#include "ledit.h"

#define TTY_LINE 96
#define EINTR 4

/* A terminal: canonical line editing on top of a byte backend (telnet socket,
 * later the LCD + keyboard and the serial port). Used by one session task. */
struct tty_ops {
    int  (*getc)(void *ctx);                           /* next input byte; -1 when hung up */
    void (*write)(void *ctx, const char *s, size_t n);
};

struct tty {
    const struct tty_ops *ops;
    void *ctx;
    struct ledit ed;
    char line[TTY_LINE];
    uint16_t rpos, rlen;             /* finished line not yet read */
};

void tty_init(struct tty *t, const struct tty_ops *ops, void *ctx);
int  tty_read(struct tty *t, char *buf, size_t n);    /* bytes; 0 at end of file; -EINTR on Ctrl-C */
void tty_write(struct tty *t, const char *s, size_t n);   /* "\n" becomes "\r\n" */

#endif
