/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "vt.h"

static void cell(struct vt *v, int col, int row)
{
    if (v->draw) v->draw(v->ctx, col, row, v->grid[row][col], col == v->x && row == v->y);
}

void vt_redraw(struct vt *v)
{
    for (int r = 0; r < VT_ROWS; ++r)
        for (int c = 0; c < VT_COLS; ++c) cell(v, c, r);
}

static void clear_rows(struct vt *v, int from, int to)
{
    for (int r = from; r < to; ++r)
        for (int c = 0; c < VT_COLS; ++c) v->grid[r][c] = ' ';
}

void vt_init(struct vt *v, void (*draw)(void *ctx, int col, int row, char c, int cursor), void *ctx)
{
    v->x = v->y = v->esc = v->param = 0;
    v->draw = draw;
    v->ctx = ctx;
    clear_rows(v, 0, VT_ROWS);
    vt_redraw(v);
}

/* Move the cursor, repainting the cells it leaves and enters. */
static void move(struct vt *v, int x, int y)
{
    int ox = v->x, oy = v->y;
    v->x = (uint8_t)x;
    v->y = (uint8_t)y;
    if (ox != x || oy != y) {
        if (ox < VT_COLS) cell(v, ox, oy);
        if (x < VT_COLS) cell(v, x, y);
    }
}

static void newline(struct vt *v)
{
    if (v->y + 1 < VT_ROWS) { move(v, v->x, v->y + 1); return; }
    for (int r = 1; r < VT_ROWS; ++r)
        for (int c = 0; c < VT_COLS; ++c) v->grid[r - 1][c] = v->grid[r][c];
    clear_rows(v, VT_ROWS - 1, VT_ROWS);
    vt_redraw(v);
}

static void put(struct vt *v, char c)
{
    if (v->x >= VT_COLS) { v->x = 0; newline(v); }  /* deferred wrap */
    v->grid[v->y][v->x] = c;
    cell(v, v->x, v->y);
    move(v, v->x + 1, v->y);
}

static void escape(struct vt *v, char c)
{
    if (v->esc == 1) {
        v->esc = c == '[' ? 2 : 0;
        v->param = 0;
        return;
    }
    if (c >= '0' && c <= '9') { v->param = (uint8_t)(v->param * 10 + (c - '0')); return; }
    if (c == ';') return;
    if (c < 0x40 || c > 0x7e) return;
    v->esc = 0;
    if (c == 'H') move(v, 0, 0);
    else if (c == 'J' && v->param == 2) { clear_rows(v, 0, VT_ROWS); vt_redraw(v); }
    else if (c == 'K') {
        for (int col = v->x; col < VT_COLS; ++col) { v->grid[v->y][col] = ' '; cell(v, col, v->y); }
    }
}

void vt_write(struct vt *v, const char *s, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        char c = s[i];
        if (v->esc) { escape(v, c); continue; }
        switch (c) {
        case '\r': move(v, 0, v->y); break;
        case '\n': newline(v); break;
        case '\b': if (v->x) move(v, (v->x >= VT_COLS ? VT_COLS : v->x) - 1, v->y); break;
        case '\t': do put(v, ' '); while (v->x % 4 && v->x < VT_COLS); break;
        case '\a': break;
        case 0x1b: v->esc = 1; break;
        default:
            if ((unsigned char)c >= 0x20 && (unsigned char)c < 0x7f) put(v, c);
            break;
        }
    }
}

size_t vt_snapshot(const struct vt *v, char *buf, size_t size)
{
    size_t n = 0;
    for (int r = 0; r < VT_ROWS; ++r) {
        int end = VT_COLS;
        while (end > 0 && v->grid[r][end - 1] == ' ') --end;
        for (int c = 0; c < end && n < size; ++c) buf[n++] = v->grid[r][c];
        if (n < size) buf[n++] = '\n';
    }
    return n;
}
