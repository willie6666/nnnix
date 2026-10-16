/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdarg.h>
#include "stdio.h"
#include "fmt.h"
#include "string.h"
#include "sys.h"

struct chunk { char buf[32]; int len; };

static void flush(struct chunk *c)
{
    if (c->len) sys_write(1, c->buf, (size_t)c->len);
    c->len = 0;
}

static void put(void *ctx, char ch)
{
    struct chunk *c = ctx;
    c->buf[c->len++] = ch;
    if (c->len == (int)sizeof c->buf) flush(c);
}

int vprintf(const char *fmt, va_list ap)
{
    struct chunk c = { .len = 0 };
    int n = nn_vformat(put, &c, fmt, ap);
    flush(&c);
    return n;
}

int printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vprintf(fmt, ap);
    va_end(ap);
    return n;
}

int fputs_fd(const char *s, int fd) { return sys_write(fd, s, strlen(s)); }
