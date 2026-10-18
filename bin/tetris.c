/* SPDX-License-Identifier: GPL-3.0-or-later */
/* tetris: left and right move (held: they repeat), up or A turns, down drops
 * faster. Full rows go; every 10 rows the level, and the speed, goes up.
 * Keys: bin/game.h. */
#include "game.h"
#include "../lib/stdio.h"
#include "../lib/string.h"
#include "../lib/sys.h"

#define COLS 10
#define ROWS 20
#define BX 49                         /* the board's first cell, 3 pixels a cell */
#define BY 2
#define FULL ((1u << COLS) - 1u)

/* The pieces in their n x n boxes, bit r * 4 + c for row r, column c. */
static const struct { uint16_t bits; uint8_t n; } shapes[7] = {
    { 0x00f0, 4 },                    /* I: row 1 */
    { 0x0033, 2 },                    /* O */
    { 0x0072, 3 },                    /* T: .X. XXX */
    { 0x0036, 3 },                    /* S: .XX XX. */
    { 0x0063, 3 },                    /* Z: XX. .XX */
    { 0x0071, 3 },                    /* J: X.. XXX */
    { 0x0074, 3 },                    /* L: ..X XXX */
};

static uint16_t board[ROWS];          /* bit c: column c is taken */
static int quit;

struct piece { int kind, turns, x, y; };

static uint16_t cells(const struct piece *p)
{
    uint16_t b = shapes[p->kind].bits;
    int n = shapes[p->kind].n;
    for (int t = 0; t < (p->turns & 3); ++t) {     /* a quarter turn clockwise: new[r][c] = old[n-1-c][r] */
        uint16_t r = 0;
        for (int row = 0; row < n; ++row)
            for (int col = 0; col < n; ++col)
                if (b >> ((n - 1 - col) * 4 + row) & 1u) r |= (uint16_t)(1u << (row * 4 + col));
        b = r;
    }
    return b;
}

static int fits(const struct piece *p)
{
    uint16_t b = cells(p);
    for (int i = 0; i < 16; ++i) {
        if (!(b >> i & 1u)) continue;
        int x = p->x + i % 4, y = p->y + i / 4;
        if (x < 0 || x >= COLS || y >= ROWS || (y >= 0 && board[y] >> x & 1u)) return 0;
    }
    return 1;
}

static void cell(int x, int y, int px, int py, int pitch)
{
    game_fill(px + x * pitch, py + y * pitch, pitch - 1, pitch - 1, 1);
}

static void draw_piece(const struct piece *p, int px, int py, int pitch)
{
    uint16_t b = cells(p);
    for (int i = 0; i < 16; ++i)
        if (b >> i & 1u && p->y + i / 4 >= 0) cell(p->x + i % 4, p->y + i / 4, px, py, pitch);
}

static void draw(const struct piece *p, int next, unsigned score, unsigned lines, unsigned level)
{
    game_clear();
    game_big(4, 2, "TETRIS");
    game_text(4, 16, "SCORE");
    game_number(4, 23, score);
    game_text(4, 32, "LINES");
    game_number(4, 39, lines);
    game_text(4, 48, "LEVEL");
    game_number(4, 55, level);
    game_box(BX - 2, 0, COLS * 3 + 4, GAME_H);
    for (int y = 0; y < ROWS; ++y)
        for (int x = 0; x < COLS; ++x)
            if (board[y] >> x & 1u) cell(x, y, BX, BY, 3);
    draw_piece(p, BX, BY, 3);
    game_text(90, 2, "NEXT");
    struct piece n = { next, 0, 0, 0 };
    draw_piece(&n, 90, 10, 4);
    game_text(84, 34, "UP TURN");
    game_text(84, 41, "DN DROP");
    game_text(84, 52, "Q QUIT");
}

/* Lock p into the board; the rows it filled go. How many. */
static int lock(const struct piece *p)
{
    uint16_t b = cells(p);
    for (int i = 0; i < 16; ++i)
        if (b >> i & 1u && p->y + i / 4 >= 0) board[p->y + i / 4] |= (uint16_t)(1u << (p->x + i % 4));
    int cleared = 0;
    for (int y = ROWS - 1; y >= 0;) {
        if (board[y] != FULL) { --y; continue; }
        memmove(&board[1], &board[0], (size_t)y * sizeof board[0]);
        board[0] = 0;
        ++cleared;
    }
    return cleared;
}

static unsigned play(void)
{
    static const unsigned points[5] = { 0, 40, 100, 300, 1200 };
    memset(board, 0, sizeof board);
    unsigned score = 0, lines = 0, level = 0;
    int next = (int)(game_rand() % 7);
    struct piece p = { 0, 0, 0, 0 };
    int spawn = 1;
    uint32_t now = sys_uptime(), fall_at = now, repeat_at = 0;
    int changed = 1;                  /* redrawn only then: a whole picture costs some CPU */
    game_pressed();
    for (;;) {
        if (spawn) {
            p = (struct piece){ next, 0, 3, 0 };
            next = (int)(game_rand() % 7);
            spawn = 0;
            if (!fits(&p)) return score;               /* no room for it: the end */
            fall_at = now + (level < 10 ? 700u - level * 60u : 100u);
        }
        if (changed) {
            draw(&p, next, score, lines, level);
            game_show_until(now += 20u);
        } else {
            game_wait_until(now += 20u);
        }
        struct piece was = p;
        unsigned was_score = score;
        unsigned pressed = game_pressed(), held = game_held();
        if (pressed & GAME_QUIT) { quit = 1; return score; }
        int dx = 0;
        if (pressed & (GAME_LEFT | GAME_RIGHT)) {
            dx = pressed & GAME_LEFT ? -1 : 1;
            repeat_at = now + 180u;
        } else if (held & (GAME_LEFT | GAME_RIGHT) && (int32_t)(now - repeat_at) >= 0) {
            dx = held & GAME_LEFT ? -1 : 1;
            repeat_at = now + 60u;
        }
        if (dx) { p.x += dx; if (!fits(&p)) p.x -= dx; }
        if (pressed & (GAME_UP | GAME_A)) {
            static const int8_t kicks[5] = { 0, -1, 1, -2, 2 };
            struct piece t = p;
            t.turns++;
            for (int k = 0; k < 5; ++k) {
                t.x = p.x + kicks[k];
                if (fits(&t)) { p = t; break; }
            }
        }
        int soft = held & GAME_DOWN;
        if (soft || (int32_t)(now - fall_at) >= 0) {
            fall_at = now + (soft ? 40u : level < 10 ? 700u - level * 60u : 100u);
            p.y++;
            if (fits(&p)) {
                score += soft ? 1u : 0u;
            } else {
                p.y--;
                int n = lock(&p);
                lines += (unsigned)n;
                score += points[n] * (level + 1u);
                level = lines / 10u;
                spawn = 1;
            }
        }
        changed = spawn || score != was_score || p.x != was.x || p.y != was.y || p.turns != was.turns;
    }
}

int cmd_tetris(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (game_open("tetris")) return 1;
    unsigned best = 0, score;
    do score = play(); while (!quit && game_over(score, &best));
    if (score > best) best = score;
    game_close();
    printf("tetris: best %u\n", best);
    return 0;
}
