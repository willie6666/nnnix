/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <string.h>
#include "../../kernel/vt.h"
#include "../../drivers/kbd.h"
#include "check.h"

static char screen[VT_ROWS][VT_COLS];
static int cursor_col = -1, cursor_row = -1, draws;

static void draw(void *ctx, int col, int row, char c, int cursor)
{
    (void)ctx;
    CHECK(col >= 0 && col < VT_COLS && row >= 0 && row < VT_ROWS);
    screen[row][col] = c;
    if (cursor) { cursor_col = col; cursor_row = row; }
    draws++;
}

#define WRITE(v, lit) vt_write((v), (lit), sizeof(lit) - 1)

static int screen_matches_grid(const struct vt *v)
{
    return memcmp(screen, v->grid, sizeof screen) == 0;
}

static void test_vt(void)
{
    struct vt v;
    char snap[256];
    vt_init(&v, draw, 0);
    CHECK_EQ_INT(draws, VT_ROWS * VT_COLS);
    WRITE(&v, "nnnix$ ls\r\n");
    CHECK(strncmp(v.grid[0], "nnnix$ ls", 9) == 0);
    CHECK_EQ_INT(v.x, 0);
    CHECK_EQ_INT(v.y, 1);
    CHECK(cursor_col == 0 && cursor_row == 1);

    /* Backspace echo "\b \b" erases the last character. */
    WRITE(&v, "abc\b \b");
    CHECK(strncmp(v.grid[1], "ab ", 3) == 0);
    CHECK_EQ_INT(v.x, 2);

    /* Exactly 21 characters fill the row without scrolling until the next one. */
    WRITE(&v, "\r\n123456789012345678901");
    CHECK_EQ_INT(v.y, 2);
    CHECK_EQ_INT(v.x, VT_COLS);
    WRITE(&v, "X");
    CHECK_EQ_INT(v.y, 3);
    CHECK_EQ_INT(v.grid[3][0], 'X');

    /* Scrolling moves everything up and keeps the screen in sync. */
    WRITE(&v, "\r\n4\r\n5\r\n6\r\n7\r\n8\r\n9\r\n");
    CHECK_EQ_INT(v.y, VT_ROWS - 1);
    size_t n = vt_snapshot(&v, snap, sizeof snap);
    snap[n] = 0;
    CHECK(strstr(snap, "nnnix$ ls") == NULL);       /* scrolled off */
    CHECK(strstr(snap, "4\n5\n6\n7\n8\n9\n") != NULL);
    CHECK(screen_matches_grid(&v));

    /* ESC [ H ESC [ 2 J clears; other sequences are swallowed; ESC [ K clears to EOL. */
    WRITE(&v, "\033[H\033[2Jtop\033[1;31mred\033[0m");
    CHECK(strncmp(v.grid[0], "topred ", 7) == 0);
    CHECK(v.grid[1][0] == ' ');
    WRITE(&v, "\r\033[3C\033[K");                   /* CSI C is ignored, so K clears from column 0 */
    CHECK(v.grid[0][0] == ' ');
    WRITE(&v, "\ta\tb");
    CHECK(v.grid[0][4] == 'a' && v.grid[0][8] == 'b');
    CHECK(screen_matches_grid(&v));
}

static void test_kbd(void)
{
    struct kbd k = { 0 };
    uint8_t out[16];
    uint8_t press_a[8] = { 0, 0, 0x04 }, shift_a[8] = { 0x02, 0, 0x04 }, none[8] = { 0 };
    uint8_t ctrl_c[8] = { 0x01, 0, 0x06 }, up[8] = { 0, 0, 0x52 }, caps[8] = { 0, 0, 0x39 };
    uint8_t enter[8] = { 0, 0, 0x28 }, bksp[8] = { 0, 0, 0x2a }, two[8] = { 0, 0, 0x04, 0x05 };

    CHECK_EQ_INT(kbd_report(&k, press_a, 0, out, 16), 1);
    CHECK_EQ_INT(out[0], 'a');
    CHECK_EQ_INT(kbd_report(&k, press_a, 10, out, 16), 0);     /* still held: no new byte */
    CHECK_EQ_INT(kbd_repeat(&k, 400, out, 16), 0);
    CHECK_EQ_INT(kbd_repeat(&k, 500, out, 16), 1);
    CHECK_EQ_INT(out[0], 'a');
    CHECK_EQ_INT(kbd_repeat(&k, 520, out, 16), 0);
    CHECK_EQ_INT(kbd_repeat(&k, 550, out, 16), 1);
    CHECK_EQ_INT(kbd_report(&k, none, 560, out, 16), 0);
    CHECK_EQ_INT(kbd_repeat(&k, 2000, out, 16), 0);            /* released: no repeat */

    CHECK_EQ_INT(kbd_report(&k, shift_a, 0, out, 16), 1);
    CHECK_EQ_INT(out[0], 'A');
    kbd_report(&k, none, 0, out, 16);
    CHECK_EQ_INT(kbd_report(&k, ctrl_c, 0, out, 16), 1);
    CHECK_EQ_INT(out[0], 0x03);
    kbd_report(&k, none, 0, out, 16);
    CHECK_EQ_INT(kbd_report(&k, up, 0, out, 16), 3);
    CHECK(memcmp(out, "\033[A", 3) == 0);
    kbd_report(&k, none, 0, out, 16);
    CHECK_EQ_INT(kbd_report(&k, enter, 0, out, 16), 1);
    CHECK_EQ_INT(out[0], '\r');
    kbd_report(&k, none, 0, out, 16);
    CHECK_EQ_INT(kbd_report(&k, bksp, 0, out, 16), 1);
    CHECK_EQ_INT(out[0], 0x7f);
    kbd_report(&k, none, 0, out, 16);

    CHECK_EQ_INT(kbd_report(&k, caps, 0, out, 16), 0);        /* Caps Lock toggles, no output */
    kbd_report(&k, none, 0, out, 16);
    CHECK_EQ_INT(kbd_report(&k, press_a, 0, out, 16), 1);
    CHECK_EQ_INT(out[0], 'A');
    kbd_report(&k, none, 0, out, 16);
    kbd_report(&k, caps, 0, out, 16);
    kbd_report(&k, none, 0, out, 16);

    /* Rolling over: 'a' held, then 'b' pressed: only 'b' is new and now repeats. */
    kbd_report(&k, press_a, 0, out, 16);
    CHECK_EQ_INT(kbd_report(&k, two, 100, out, 16), 1);
    CHECK_EQ_INT(out[0], 'b');
    CHECK_EQ_INT(kbd_repeat(&k, 600, out, 16), 1);
    CHECK_EQ_INT(out[0], 'b');
}

int main(void)
{
    test_vt();
    test_kbd();
    return check_summary("test_console");
}
