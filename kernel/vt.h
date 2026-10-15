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
 * under the cursor. */
#define VT_COLS 21
#define VT_ROWS 8

struct vt {
    char grid[VT_ROWS][VT_COLS];
    uint8_t x, y, esc, param;
    void (*draw)(void *ctx, int col, int row, char c, int cursor);
    void *ctx;
};

void vt_init(struct vt *v, void (*draw)(void *ctx, int col, int row, char c, int cursor), void *ctx);
void vt_write(struct vt *v, const char *s, size_t n);
void vt_redraw(struct vt *v);
/* The grid as text: rows joined by '\n', trailing blanks trimmed. Returns length. */
size_t vt_snapshot(const struct vt *v, char *buf, size_t size);

#endif
