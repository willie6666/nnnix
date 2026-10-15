/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "fmt.h"
#include <stdint.h>

struct out { void (*sink)(void *ctx, char c); void *ctx; size_t len; };

static void put(struct out *o, char c)
{
    o->sink(o->ctx, c);
    o->len++;
}

static void pad(struct out *o, char c, int n) { while (n-- > 0) put(o, c); }

static void number(struct out *o, uint32_t v, unsigned base, int upper, int negative,
                   int width, int zero, int left)
{
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[11];
    int n = 0;
    do { tmp[n++] = digits[v % base]; v /= base; } while (v);
    int len = n + negative;
    if (!left && !zero) pad(o, ' ', width - len);
    if (negative) put(o, '-');
    if (!left && zero) pad(o, '0', width - len);
    while (n) put(o, tmp[--n]);
    if (left) pad(o, ' ', width - len);
}

int nn_vformat(void (*sink)(void *ctx, char c), void *ctx, const char *fmt, va_list ap)
{
    struct out o = { sink, ctx, 0 };
    for (; *fmt; ++fmt) {
        if (*fmt != '%') { put(&o, *fmt); continue; }
        int left = 0, zero = 0, width = 0;
        for (;; ++fmt) {
            if (fmt[1] == '-') left = 1;
            else if (fmt[1] == '0') zero = 1;
            else break;
        }
        ++fmt;
        if (*fmt == '*') { width = va_arg(ap, int); ++fmt; }
        else while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        while (*fmt == 'l') ++fmt;
        switch (*fmt) {
        case 'd': case 'i': {
            int32_t v = va_arg(ap, int32_t);
            number(&o, v < 0 ? -(uint32_t)v : (uint32_t)v, 10, 0, v < 0, width, zero, left);
            break;
        }
        case 'u': number(&o, va_arg(ap, uint32_t), 10, 0, 0, width, zero, left); break;
        case 'x': number(&o, va_arg(ap, uint32_t), 16, 0, 0, width, zero, left); break;
        case 'X': number(&o, va_arg(ap, uint32_t), 16, 1, 0, width, zero, left); break;
        case 'p':
            put(&o, '0'); put(&o, 'x');
            number(&o, (uint32_t)(uintptr_t)va_arg(ap, void *), 16, 0, 0, 8, 1, 0);
            break;
        case 'c': {
            if (!left) pad(&o, ' ', width - 1);
            put(&o, (char)va_arg(ap, int));
            if (left) pad(&o, ' ', width - 1);
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            int n = 0;
            while (s[n]) ++n;
            if (!left) pad(&o, ' ', width - n);
            while (*s) put(&o, *s++);
            if (left) pad(&o, ' ', width - n);
            break;
        }
        case '%': put(&o, '%'); break;
        case '\0': --fmt; break;
        default: put(&o, '%'); put(&o, *fmt); break;
        }
    }
    return (int)o.len;
}

struct bounded { char *buf; size_t size, len; };

static void to_buffer(void *ctx, char c)
{
    struct bounded *b = ctx;
    if (b->len + 1 < b->size) b->buf[b->len] = c;
    b->len++;
}

int nn_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    struct bounded b = { buf, size, 0 };
    int n = nn_vformat(to_buffer, &b, fmt, ap);
    if (size) buf[b.len < size ? b.len : size - 1] = '\0';
    return n;
}

int nn_snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = nn_vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}
