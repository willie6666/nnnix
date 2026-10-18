/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <string.h>
#include "../../kernel/vt.h"
#include "../../drivers/kbd.h"
#include "check.h"

static char screen[VT_ROWS_MAX][VT_COLS_MAX];
static int cursor_col = -1, cursor_row = -1, draws;

static void draw(void *ctx, int col, int row, char c, int cursor)
{
    (void)ctx;
    CHECK(col >= 0 && col < VT_COLS_MAX && row >= 0 && row < VT_ROWS_MAX);
    screen[row][col] = c;
    if (cursor) { cursor_col = col; cursor_row = row; }
    draws++;
}

#define WRITE(v, lit) vt_write((v), (lit), sizeof(lit) - 1)

static int screen_matches_grid(const struct vt *v)
{
    for (int r = 0; r < v->rows; ++r)
        if (memcmp(screen[r], v->grid[r], v->cols)) return 0;
    return 1;
}

/* A scrollback for the resize test: rows in order, newest last. */
static char back[16][VT_COLS_MAX];
static int nback;

static void scrolled(void *ctx, const char *row, int n)
{
    (void)ctx;
    CHECK_EQ_INT(n, VT_COLS_MAX);
    if (nback < 16) memcpy(back[nback++], row, VT_COLS_MAX);
}

static int unscroll(void *ctx, char *row, int n)
{
    (void)ctx;
    if (!nback) return 0;
    memcpy(row, back[--nback], (size_t)n);
    return 1;
}

static void test_resize(void)
{
    struct vt v;
    vt_init(&v, draw, 0);
    v.scrolled = scrolled;
    v.unscroll = unscroll;
    for (int i = 0; i < 10; ++i) {                   /* 0..9: 0 and 1 scroll off */
        char line[8] = { (char)('0' + i), '\r', '\n' };
        vt_write(&v, line, i < 9 ? 3 : 1);
    }
    CHECK_EQ_INT(nback, 2);
    CHECK(back[0][0] == '0' && back[1][0] == '1' && back[1][1] == ' ');
    CHECK(v.grid[0][0] == '2' && v.grid[7][0] == '9' && v.y == 7 && v.x == 1);

    /* Wider and taller: two rows come back from the scrollback above the text. */
    vt_resize(&v, 32, 10);
    CHECK_EQ_INT(nback, 0);
    CHECK(v.cols == 32 && v.rows == 10 && v.y == 9 && v.x == 1);
    CHECK(v.grid[0][0] == '0' && v.grid[1][0] == '1' && v.grid[9][0] == '9');
    CHECK(screen_matches_grid(&v));
    vt_write(&v, "\r123456789012345678901234567890AB", 33);   /* 32 fit on a row now */
    CHECK(v.y == 9 && v.x == 32 && v.grid[9][31] == 'B');

    /* Narrower and shorter: the top rows leave, the long row keeps its tail. */
    vt_resize(&v, 21, 8);
    CHECK_EQ_INT(nback, 2);
    CHECK(back[0][0] == '0' && back[1][0] == '1');
    CHECK(v.y == 7 && v.x == 21 && v.grid[7][31] == 'B' && v.grid[0][0] == '2');
    CHECK(screen_matches_grid(&v));
    vt_write(&v, "z", 1);                            /* the pending wrap at 21 columns */
    CHECK(v.y == 7 && v.grid[7][0] == 'z' && v.x == 1);
    CHECK_EQ_INT(nback, 3);
    CHECK(back[2][0] == '2' && v.grid[6][0] == '1' && v.grid[6][31] == 'B');
    vt_resize(&v, 21, 8);                            /* the same size: nothing moves */
    CHECK(v.y == 7 && v.grid[7][0] == 'z');
    vt_resize(&v, 40, 8);                            /* too wide: refused */
    CHECK_EQ_INT(v.cols, 21);
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

static void test_console_keys(void)
{
    struct kbd k = { 0 };
    uint8_t out[16];
    uint8_t pgup[8] = { 0, 0, 0x4b }, sh_pgdn[8] = { 0x02, 0, 0x4e }, sh_up[8] = { 0x20, 0, 0x52 };
    uint8_t sh_down[8] = { 0x02, 0, 0x51 }, up[8] = { 0, 0, 0x52 }, minus[8] = { 0x01, 0, 0x2d };
    uint8_t equal[8] = { 0x10, 0, 0x2e }, zero[8] = { 0x01, 0, 0x27 }, none[8] = { 0 };
    uint8_t plain_minus[8] = { 0, 0, 0x2d };
    CHECK(kbd_report(&k, pgup, 0, out, 16) == 1 && out[0] == KBD_PAGE_UP);
    kbd_report(&k, none, 0, out, 16);
    CHECK(kbd_report(&k, sh_pgdn, 0, out, 16) == 1 && out[0] == KBD_PAGE_DOWN);
    kbd_report(&k, none, 0, out, 16);
    CHECK(kbd_report(&k, sh_up, 0, out, 16) == 1 && out[0] == KBD_LINE_UP);   /* right Shift */
    kbd_report(&k, none, 0, out, 16);
    CHECK(kbd_report(&k, sh_down, 0, out, 16) == 1 && out[0] == KBD_LINE_DOWN);
    kbd_report(&k, none, 0, out, 16);
    CHECK(kbd_report(&k, up, 0, out, 16) == 3 && out[0] == 0x1b);   /* Up alone stays a sequence */
    kbd_report(&k, none, 0, out, 16);
    CHECK(kbd_report(&k, minus, 0, out, 16) == 1 && out[0] == KBD_TEXT_SMALL);
    kbd_report(&k, none, 0, out, 16);
    CHECK(kbd_report(&k, equal, 0, out, 16) == 1 && out[0] == KBD_TEXT_LARGE);   /* right Ctrl */
    kbd_report(&k, none, 0, out, 16);
    CHECK(kbd_report(&k, zero, 0, out, 16) == 1 && out[0] == KBD_TEXT_LARGE);
    kbd_report(&k, none, 0, out, 16);
    CHECK(kbd_report(&k, plain_minus, 0, out, 16) == 1 && out[0] == '-');
    kbd_report(&k, none, 0, out, 16);
    CHECK(kbd_report(&k, sh_pgdn, 0, out, 16) == 1);                  /* held: repeats */
    CHECK(kbd_repeat(&k, KBD_REPEAT_DELAY, out, 16) == 1 && out[0] == KBD_PAGE_DOWN);
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
    test_resize();
    test_kbd();
    test_console_keys();
    return check_summary("test_console");
}
