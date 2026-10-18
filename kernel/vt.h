/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NNNIX_VT_H
#define NNNIX_VT_H
#include <stddef.h>
#include <stdint.h>

/* A small text terminal: a character grid with a cursor, independent of the
 * display (host tested). Understands CR, LF, BS, TAB, BEL (ignored), automatic
 * wrap and scroll, and the escape sequences ESC [ H (home), ESC [ 2 J (clear),
 * ESC [ K (clear to end of line); other sequences are swallowed.
 * `draw` is called for every cell that changes, with cursor = 1 for the cell
 * under the cursor; `redraw`, when set, instead of a draw for every cell.
 *
 * The grid holds VT_COLS_MAX x VT_ROWS_MAX characters; the terminal uses
 * cols x rows of it (vt_resize: the console's two font sizes). Columns past
 * cols keep what they held, so text comes back when the terminal widens. A
 * row that scrolls off the top goes to `scrolled` (the scrollback), and when
 * the terminal grows `unscroll` may hand the latest one back. */
#define VT_COLS 21                   /* the size after vt_init */
#define VT_ROWS 8
#define VT_COLS_MAX 32
#define VT_ROWS_MAX 10

struct vt {
    char grid[VT_ROWS_MAX][VT_COLS_MAX];
    uint8_t cols, rows, x, y, esc, param;
    void (*draw)(void *ctx, int col, int row, char c, int cursor);
    void (*redraw)(void *ctx);                                   /* optional */
    void (*scrolled)(void *ctx, const char *row, int n);         /* optional: n = VT_COLS_MAX */
    int (*unscroll)(void *ctx, char *row, int n);                /* optional: 1 when it filled row */
    void *ctx;
};

void vt_init(struct vt *v, void (*draw)(void *ctx, int col, int row, char c, int cursor), void *ctx);
void vt_write(struct vt *v, const char *s, size_t n);
void vt_redraw(struct vt *v);
/* Another size (at most the maximum): the text stays at the bottom, rows
 * leave the top through `scrolled` or come back through `unscroll`. Redraws. */
void vt_resize(struct vt *v, int cols, int rows);
/* The grid as text: rows joined by '\n', trailing blanks trimmed. Returns length. */
size_t vt_snapshot(const struct vt *v, char *buf, size_t size);

#endif
