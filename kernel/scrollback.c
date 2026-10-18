/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "scrollback.h"

void scrollback_init(struct scrollback *s, char *buf, uint32_t size)
{
    *s = (struct scrollback){ .buf = buf, .size = buf ? size : 0 };
}

static void put(struct scrollback *s, char c)
{
    if (s->end - s->start == s->size) {              /* full: the oldest byte goes */
        char old = s->buf[s->start++ % s->size];
        if (old == '\n') { s->newlines--; s->cut = 0; } else s->cut = 1;
    }
    s->buf[s->end++ % s->size] = c;
    if (c == '\n') s->newlines++;
}

void scrollback_push(struct scrollback *s, const char *row, int n)
{
    if (!s->size) return;
    while (n > 0 && row[n - 1] == ' ') --n;
    for (int i = 0; i < n; ++i) put(s, row[i]);
    put(s, '\n');
}

uint32_t scrollback_lines(const struct scrollback *s) { return s->newlines - (s->cut && s->newlines ? 1u : 0u); }

/* Where line `back` starts; *end just after its '\n'. */
static uint32_t find(const struct scrollback *s, uint32_t back, uint32_t *end)
{
    uint32_t e = s->end;
    for (;;) {
        uint32_t start = e - 1;
        while (start > s->start && s->buf[(start - 1) % s->size] != '\n') --start;
        if (!back--) { *end = e; return start; }
        e = start;
    }
}

int scrollback_line(const struct scrollback *s, uint32_t back, char *row, int n)
{
    if (back >= scrollback_lines(s)) return 0;
    uint32_t end, i = find(s, back, &end);
    int k = 0;
    for (; i < end - 1 && k < n; ++i) row[k++] = s->buf[i % s->size];
    while (k < n) row[k++] = ' ';
    return 1;
}

int scrollback_pop(struct scrollback *s, char *row, int n)
{
    if (!scrollback_line(s, 0, row, n)) return 0;
    uint32_t end;
    s->end = find(s, 0, &end);
    s->newlines--;
    return 1;
}
