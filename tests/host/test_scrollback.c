/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The console's scrollback ring: lines in order, trimmed, the oldest dropping
 * out when the ring is full (never shown cut), lines taken back out. */
#include <stdio.h>
#include <string.h>
#include "../../kernel/scrollback.h"
#include "check.h"

#define W 32

static int line_is(const struct scrollback *s, uint32_t back, const char *text)
{
    char row[W], want[W];
    memset(want, ' ', W);
    memcpy(want, text, strlen(text));
    return scrollback_line(s, back, row, W) && !memcmp(row, want, W);
}

static void push(struct scrollback *s, const char *text)
{
    char row[W];
    memset(row, ' ', W);
    memcpy(row, text, strlen(text));
    scrollback_push(s, row, W);
}

int main(void)
{
    struct scrollback s;
    char ring[64], row[W];
    scrollback_init(&s, 0, 64);                      /* no ring: nothing kept */
    push(&s, "lost");
    CHECK_EQ_INT(scrollback_lines(&s), 0);
    CHECK(!scrollback_line(&s, 0, row, W) && !scrollback_pop(&s, row, W));

    scrollback_init(&s, ring, sizeof ring);
    push(&s, "first");
    push(&s, "");
    push(&s, "third line");
    CHECK_EQ_INT(scrollback_lines(&s), 3);
    CHECK_EQ_INT(s.end, 6 + 1 + 11);                 /* trimmed: "first\n" "\n" "third line\n" */
    CHECK(line_is(&s, 0, "third line") && line_is(&s, 1, "") && line_is(&s, 2, "first"));
    CHECK(!scrollback_line(&s, 3, row, W));

    /* Taken back out, newest first. */
    CHECK(scrollback_pop(&s, row, W) && !memcmp(row, "third line ", 11));
    CHECK_EQ_INT(scrollback_lines(&s), 2);
    CHECK(line_is(&s, 0, "") && line_is(&s, 1, "first"));
    push(&s, "again");
    CHECK(line_is(&s, 0, "again") && line_is(&s, 2, "first"));

    /* Full: the oldest go; a line cut at the front is not shown. 64 bytes hold
     * the last 64 of the stream; "lineNN\n" is 7 bytes. */
    for (int i = 0; i < 20; ++i) {
        char text[8];
        snprintf(text, sizeof text, "line%02d", i);
        push(&s, text);
    }
    uint32_t n = scrollback_lines(&s);
    CHECK_EQ_INT(n, 9);                              /* 64 / 7: 9 whole lines and a cut one */
    CHECK(line_is(&s, 0, "line19") && line_is(&s, 8, "line11"));
    CHECK(!scrollback_line(&s, 9, row, W));
    CHECK_EQ_INT(s.end - s.start, 64);
    for (uint32_t i = 0; i < n; ++i) CHECK(scrollback_pop(&s, row, W));
    CHECK(!scrollback_pop(&s, row, W));              /* the cut one is never handed out */
    push(&s, "after");
    CHECK_EQ_INT(scrollback_lines(&s), 1);
    CHECK(line_is(&s, 0, "after"));

    /* A row of the full width, and one longer than the ring (a 16-byte ring). */
    char tiny[16];
    scrollback_init(&s, tiny, sizeof tiny);
    push(&s, "0123456789abcdefghijklmnopqrstuv");
    CHECK_EQ_INT(scrollback_lines(&s), 0);
    push(&s, "ok");
    CHECK_EQ_INT(scrollback_lines(&s), 1);
    CHECK(line_is(&s, 0, "ok"));
    return check_summary("test_scrollback");
}
