/* SPDX-License-Identifier: GPL-3.0-or-later */
/* snake: eat, grow, do not hit the walls or yourself. Directions turn; it
 * speeds up as it grows. Keys: bin/game.h. */
#include "game.h"
#include "../lib/stdio.h"
#include "../lib/string.h"
#include "../lib/sys.h"

#define W 32                          /* cells of 4x4 pixels under a 8-pixel score bar */
#define H 14
#define TOP 8
enum { EMPTY, UP, DOWN, LEFT, RIGHT, FOOD };   /* a body cell holds the way to the next one */

static uint8_t grid[W * H];
static int quit;

static void put(int at, int kind)
{
    int x = at % W * 4, y = TOP + at / W * 4;
    grid[at] = (uint8_t)kind;
    game_fill(x, y, 4, 4, 0);
    if (kind == FOOD) { game_fill(x + 1, y, 2, 4, 1); game_fill(x, y + 1, 4, 2, 1); }
    else if (kind != EMPTY) game_fill(x, y, 4, 4, 1);
}

/* Food on an empty cell: 0, or -1 when there is none (the snake fills the
 * board: it has won). A few random tries, then the next empty cell after
 * the last try, so it always ends (review R57). */
static int food(void)
{
    int at = 0;
    for (int tries = 0; tries < 64; ++tries) {
        at = (int)(game_rand() % (W * H));
        if (grid[at] == EMPTY) { put(at, FOOD); return 0; }
    }
    for (int k = 0; k < W * H; ++k, at = (at + 1) % (W * H))
        if (grid[at] == EMPTY) { put(at, FOOD); return 0; }
    return -1;
}

static int step(int at, int dir)
{
    int x = at % W + (dir == RIGHT) - (dir == LEFT), y = at / W + (dir == DOWN) - (dir == UP);
    return x < 0 || x >= W || y < 0 || y >= H ? -1 : x + y * W;
}

static void bar(unsigned score, unsigned best)
{
    game_fill(0, 0, GAME_W, TOP, 0);
    game_text(0, 1, "SNAKE");
    game_text(40, 1, "SCORE");
    game_number(62, 1, score);
    game_text(90, 1, "BEST");
    game_number(108, 1, best);
    game_fill(0, TOP - 1, GAME_W, 1, 1);
}

static unsigned play(unsigned best)
{
    memset(grid, 0, sizeof grid);
    game_clear();
    int head = W / 2 + H / 2 * W, tail = head - 2, dir = RIGHT;
    unsigned score = 0, length = 3;
    for (int at = tail; at <= head; ++at) put(at, RIGHT);
    food();
    game_pressed();
    for (uint32_t next = sys_uptime();;) {
        bar(score, best);
        uint32_t step_ms = length < 40 ? 160u - length * 3u : 40u;
        game_show_until(next += step_ms);
        unsigned p = game_pressed();
        if (p & GAME_QUIT) { quit = 1; return score; }
        /* No turning back into itself. */
        int turn = p & GAME_UP ? UP : p & GAME_DOWN ? DOWN : p & GAME_LEFT ? LEFT : p & GAME_RIGHT ? RIGHT : 0;
        if (turn && (turn == UP || turn == DOWN) != (dir == UP || dir == DOWN)) dir = turn;
        int to = step(head, dir);
        if (to < 0 || (grid[to] != EMPTY && grid[to] != FOOD && to != tail)) return score;   /* a wall, or itself */
        int ate = grid[to] == FOOD;
        put(head, dir);                                /* the old head points to the new one */
        if (!ate) {
            int after = step(tail, grid[tail]);
            put(tail, EMPTY);
            tail = after;
        } else {
            ++length;
            score += 10;
        }
        put(to, dir);
        head = to;
        if (ate && food()) return score;              /* no room left: a full board ends the game */
    }
}

int cmd_snake(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (game_open("snake")) return 1;
    unsigned best = 0, score;
    do score = play(best); while (!quit && game_over(score, &best));
    if (score > best) best = score;
    game_close();
    printf("snake: best %u\n", best);
    return 0;
}
