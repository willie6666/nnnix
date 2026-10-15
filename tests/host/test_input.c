/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <string.h>
#include "../../drivers/ch9350.h"
#include "../../drivers/hidkey.h"
#include "check.h"

static int read_hex_fixture(const char *path, unsigned char *out, int max)
{
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return -1; }
    char line[256];
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        unsigned v;
        int off = 0, used;
        while (n < max && sscanf(line + off, "%x%n", &v, &used) == 1) { out[n++] = (unsigned char)v; off += used; }
    }
    fclose(f);
    return n;
}

static void test_synthetic(void)
{
    struct ch9350_parser p = { 0 };
    struct ch9350_event ev;
    /* Garbage, then a keyboard frame holding 'a', then a mouse frame. */
    const unsigned char stream[] = { 0x00, 0x57, 0x00, 0x57, 0xab, 0x01, 0x02, 0x00, 0x04, 0, 0, 0, 0, 0,
                                     0x57, 0xab, 0x02, 0x01, 0xfe, 0x03, 0xff, 0x57, 0xab, 0x86 };
    int events = 0, kb = 0, mouse = 0, removed = 0;
    for (size_t i = 0; i < sizeof stream; ++i) {
        if (!ch9350_feed(&p, stream[i], &ev)) continue;
        events++;
        if (ev.type == CH9350_KEYBOARD) { kb++; CHECK_EQ_INT(ev.data[0], 0x02); CHECK_EQ_INT(ev.data[2], 0x04); }
        if (ev.type == CH9350_MOUSE) {
            mouse++;
            CHECK_EQ_INT(ev.data[0], 1);
            CHECK_EQ_INT((signed char)ev.data[1], -2);
            CHECK_EQ_INT((signed char)ev.data[2], 3);
            CHECK_EQ_INT((signed char)ev.data[3], -1);
        }
        if (ev.type == CH9350_REMOVED) removed++;
    }
    CHECK_EQ_INT(events, 3);
    CHECK_EQ_INT(kb, 1);
    CHECK_EQ_INT(mouse, 1);
    CHECK_EQ_INT(removed, 1);
    CHECK_EQ_INT(p.bad_bytes, 2);     /* 0x00, then the 0x00 after the first 0x57 */
}

static void test_capture(void)
{
    unsigned char data[2048];
    int n = read_hex_fixture("fixtures/ch9350-capture.hex", data, sizeof data);
    CHECK_EQ_INT(n, 872);
    struct ch9350_parser p = { 0 };
    struct ch9350_event ev;
    int kb = 0, mouse = 0, status = 0, a = 0, enter = 0, release = 0, left = 0, right = 0, wheel = 0;
    for (int i = 0; i < n; ++i) {
        if (!ch9350_feed(&p, data[i], &ev)) continue;
        if (ev.type == CH9350_KEYBOARD) {
            kb++;
            int any = 0;
            for (int k = 2; k < 8; ++k) {
                if (ev.data[k] == 0x04) a++;
                if (ev.data[k] == 0x28) enter++;
                any |= ev.data[k];
            }
            if (!any && !ev.data[0]) release++;
        } else if (ev.type == CH9350_MOUSE) {
            mouse++;
            if (ev.data[0] & 1) left++;
            if (ev.data[0] & 2) right++;
            if (ev.data[3]) wheel++;
        } else if (ev.type == CH9350_STATUS) status++;
    }
    /* Counts verified independently by scripts/ch9350_analyze.py on the same capture. */
    CHECK_EQ_INT(kb, 20);
    CHECK_EQ_INT(a, 5);
    CHECK_EQ_INT(enter, 5);
    CHECK_EQ_INT(release, 10);
    CHECK_EQ_INT(mouse, 88);
    CHECK_EQ_INT(left, 3);
    CHECK_EQ_INT(right, 4);
    CHECK_EQ_INT(wheel, 20);
    CHECK_EQ_INT(status, 9);
    CHECK_EQ_INT(p.bad_bytes, 0);
    CHECK_EQ_INT(p.len, 0);
}

static void test_hidkey(void)
{
    CHECK_EQ_INT(hid_to_ascii(0x04, 0, 0), 'a');
    CHECK_EQ_INT(hid_to_ascii(0x04, 0x02, 0), 'A');
    CHECK_EQ_INT(hid_to_ascii(0x04, 0, 1), 'A');
    CHECK_EQ_INT(hid_to_ascii(0x04, 0x20, 1), 'a');     /* caps + right shift */
    CHECK_EQ_INT(hid_to_ascii(0x1e, 0, 1), '1');        /* caps does not shift digits */
    CHECK_EQ_INT(hid_to_ascii(0x1e, 0x02, 0), '!');
    CHECK_EQ_INT(hid_to_ascii(0x27, 0, 0), '0');
    CHECK_EQ_INT(hid_to_ascii(0x28, 0, 0), '\n');
    CHECK_EQ_INT(hid_to_ascii(0x2c, 0, 0), ' ');
    CHECK_EQ_INT(hid_to_ascii(0x2d, 0x02, 0), '_');
    CHECK_EQ_INT(hid_to_ascii(0x34, 0x02, 0), '"');
    CHECK_EQ_INT(hid_to_ascii(0x38, 0, 0), '/');
    CHECK_EQ_INT(hid_to_ascii(0x38, 0x02, 0), '?');
    CHECK_EQ_INT(hid_to_ascii(0x39, 0, 0), 0);
    CHECK(strcmp(hid_key_name(0x04), "a") == 0);
    CHECK(strcmp(hid_key_name(0x3a), "F1") == 0);
    CHECK(strcmp(hid_key_name(0x28), "Enter") == 0);
    CHECK(hid_key_name(0x00) == NULL);
}

int main(void)
{
    test_synthetic();
    test_capture();
    test_hidkey();
    return check_summary("test_input");
}
