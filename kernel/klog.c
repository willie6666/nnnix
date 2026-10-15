/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdarg.h>
#include "klog.h"
#include "../lib/fmt.h"
#include "../drivers/uart.h"
#include "../arch/nuc140/arch.h"

#define KLOG_RING 512u
static char ring[KLOG_RING];
static uint32_t ring_head;            /* total bytes ever written */
static int line_start = 1;

static void emit(const char *s, size_t n)
{
    uart0_write(s, n);
    for (size_t i = 0; i < n; ++i) ring[ring_head++ % KLOG_RING] = s[i];
}

void kprintf(const char *fmt, ...)
{
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    int n = nn_vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof buf - 1) n = sizeof buf - 1;
    const char *p = buf, *endp = buf + n;
    while (p < endp) {
        if (line_start) {
            char stamp[16];
            uint32_t now = jiffies;
            int m = nn_snprintf(stamp, sizeof stamp, "[%5u.%03u] ", now / 1000u, now % 1000u);
            emit(stamp, (size_t)m);
            line_start = 0;
        }
        const char *nl = p;
        while (nl < endp && *nl != '\n') ++nl;
        if (nl < endp) { emit(p, (size_t)(nl - p + 1)); line_start = 1; p = nl + 1; }
        else { emit(p, (size_t)(endp - p)); p = endp; }
    }
}

size_t klog_read(char *buf, size_t size)
{
    uint32_t count = ring_head < KLOG_RING ? ring_head : KLOG_RING;
    uint32_t start = ring_head - count;
    if (count > size) { start += count - size; count = size; }
    for (uint32_t i = 0; i < count; ++i) buf[i] = ring[(start + i) % KLOG_RING];
    return count;
}
