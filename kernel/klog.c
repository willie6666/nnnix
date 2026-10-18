/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdarg.h>
#include "klog.h"
#include "../lib/fmt.h"
#include "../drivers/uart.h"
#include "../arch/nuc140/arch.h"
#include "sync.h"

#define KLOG_RING 512u
static char ring[KLOG_RING];
static uint32_t ring_head;            /* total bytes ever written */
static int line_start = 1;

static void emit(const char *s, size_t n)
{
    uart0_write(s, n);
    for (size_t i = 0; i < n; ++i) ring[ring_head++ % KLOG_RING] = s[i];
}

/* Serialises whole messages between tasks; a no-op before the scheduler runs. */
static struct mutex lock;

/* "[sssss.mmm] " without the formatter: kprintf runs on the caller's stack,
 * often a program's, and a second nn_vformat under the first took 200 B more
 * of it. */
static size_t stamp(char buf[16])
{
    uint32_t now = jiffies, sec = now / 1000u, ms = now % 1000u;
    char digits[8];
    size_t n = 0, k = 0;
    do { digits[n++] = (char)('0' + sec % 10u); sec /= 10u; } while (sec);
    buf[k++] = '[';
    for (size_t i = n; i < 5; ++i) buf[k++] = ' ';
    while (n) buf[k++] = digits[--n];
    buf[k++] = '.';
    buf[k++] = (char)('0' + ms / 100u);
    buf[k++] = (char)('0' + ms / 10u % 10u);
    buf[k++] = (char)('0' + ms % 10u);
    buf[k++] = ']';
    buf[k++] = ' ';
    return k;
}

static void put_locked(const char *buf, size_t n)
{
    const char *p = buf, *endp = buf + n;
    while (p < endp) {
        if (line_start) {
            char text[16];
            emit(text, stamp(text));
            line_start = 0;
        }
        const char *nl = p;
        while (nl < endp && *nl != '\n') ++nl;
        if (nl < endp) { emit(p, (size_t)(nl - p + 1)); line_start = 1; p = nl + 1; }
        else { emit(p, (size_t)(endp - p)); p = endp; }
    }
}

void klog_write(const char *buf, size_t n)
{
    mutex_lock(&lock);
    put_locked(buf, n);
    mutex_unlock(&lock);
}

struct chunk { char buf[32]; size_t len; };

static void chunk_put(void *ctx, char c)
{
    struct chunk *k = ctx;
    k->buf[k->len++] = c;
    if (k->len == sizeof k->buf) { put_locked(k->buf, k->len); k->len = 0; }
}

void kvprintf(const char *fmt, va_list ap)
{
    struct chunk k = { .len = 0 };
    mutex_lock(&lock);
    nn_vformat(chunk_put, &k, fmt, ap);
    put_locked(k.buf, k.len);
    mutex_unlock(&lock);
}

void kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
}

size_t klog_read(char *buf, size_t size)
{
    uint32_t count = ring_head < KLOG_RING ? ring_head : KLOG_RING;
    uint32_t start = ring_head - count;
    if (count > size) { start += count - size; count = size; }
    for (uint32_t i = 0; i < count; ++i) buf[i] = ring[(start + i) % KLOG_RING];
    return count;
}

size_t klog_read_from(uint32_t *pos, char *buf, size_t size)
{
    uint32_t head = ring_head, oldest = head > KLOG_RING ? head - KLOG_RING : 0;
    if (*pos < oldest || *pos > head) *pos = oldest;
    size_t n = 0;
    while (n < size && *pos < head) buf[n++] = ring[(*pos)++ % KLOG_RING];
    return n;
}
