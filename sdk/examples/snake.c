/* SPDX-License-Identifier: GPL-3.0-or-later */
/* snake: the game, on the LCD. Arrow keys or WASD on the USB keyboard, or
 * 2 4 6 8 on the board's keypad; Esc, q or keypad 5 ends it (and the board's
 * SW_INT button always does). The score is printed at the end.
 *
 * An example of /dev/input (O_NONBLOCK: the snake moves on whether or not a
 * key comes) with /dev/lcd (a whole 1024-byte frame per step). */
#include "nnnix.h"

#define W 32                          /* cells of 4x4 pixels: 128x64 */
#define H 16
#define STEP_MS 120u
enum { EMPTY, UP, DOWN, LEFT, RIGHT, FOOD };   /* a body cell holds the way to the next one */

static uint8_t grid[W * H];
static uint8_t *frame;                /* /dev/lcd layout: byte x + 128 * page, bit 0 the top row */
static uint32_t seed;

static uint32_t rnd(void) { return seed = seed * 1664525u + 1013904223u; }

static void cell(int x, int y, int kind)
{
    uint8_t bits = (uint8_t)((kind == FOOD ? 0x06 : 0x0f) << ((y & 1) * 4)), *col = frame + x * 4 + 128 * (y / 2);
    for (int i = 0; i < 4; ++i) {
        col[i] &= (uint8_t)~(0x0f << ((y & 1) * 4));
        if (kind != EMPTY && (kind != FOOD || i == 1 || i == 2)) col[i] |= bits;
    }
}

static void put(int at, int kind)
{
    grid[at] = (uint8_t)kind;
    cell(at % W, at / W, kind);
}

static void food(void)
{
    for (;;) {
        int at = (int)(rnd() >> 8) % (W * H);
        if (grid[at] == EMPTY) { put(at, FOOD); return; }
    }
}

static int step(int at, int dir)
{
    int x = at % W, y = at / W;
    x += dir == RIGHT ? 1 : dir == LEFT ? -1 : 0;
    y += dir == DOWN ? 1 : dir == UP ? -1 : 0;
    return x < 0 || x >= W || y < 0 || y >= H ? -1 : x + y * W;
}

/* The direction a key asks for, 0 for none, -1 to quit. */
static int wanted(const struct nn_input_event *e)
{
    if (!e->value) return 0;
    if (e->type == NN_INPUT_PAD) return e->code == 2 ? UP : e->code == 8 ? DOWN : e->code == 4 ? LEFT : e->code == 6 ? RIGHT : e->code == 5 ? -1 : 0;
    if (e->type != NN_INPUT_KEY) return 0;
    switch (e->code) {
    case NN_KEY_UP: case NN_KEY_A + 'w' - 'a': return UP;
    case NN_KEY_DOWN: case NN_KEY_A + 's' - 'a': return DOWN;
    case NN_KEY_LEFT: case NN_KEY_A: return LEFT;
    case NN_KEY_RIGHT: case NN_KEY_A + 'd' - 'a': return RIGHT;
    case NN_KEY_ESC: case NN_KEY_A + 'q' - 'a': return -1;
    default: return 0;
    }
}

int main(void)
{
    int in = sys_open("/dev/input", O_RDONLY | O_NONBLOCK);
    if (in < 0) { printf("snake: /dev/input: %s\n", errstr(in)); return 1; }
    int lcd = sys_open("/dev/lcd", O_WRONLY);
    frame = malloc(1024);
    if (lcd < 0 || !frame) { printf("snake: %s\n", lcd < 0 ? errstr(lcd) : "out of memory"); return 1; }
    memset(frame, 0, 1024);
    seed = sys_uptime() * 2654435761u;
    int head = W / 2 + H / 2 * W, tail = head - 2, dir = RIGHT, length = 3, quit = 0, score = 0;
    for (int at = tail; at <= head; ++at) put(at, RIGHT);
    food();
    while (!quit) {
        sys_write(lcd, frame, 1024);
        uint32_t next = sys_uptime() + STEP_MS;
        int turn = 0;
        while (!quit && (int32_t)(sys_uptime() - next) < 0) {
            struct nn_input_event e;
            if (sys_read(in, &e, sizeof e) == (int)sizeof e) {
                int w = wanted(&e);
                if (w < 0) quit = 1;
                /* No turning back into itself; the first turn of a step counts. */
                else if (w && !turn && (w == UP || w == DOWN) != (dir == UP || dir == DOWN)) turn = w;
            } else {
                sys_sleep(10);
            }
        }
        if (turn) dir = turn;
        int to = step(head, dir);
        if (to < 0 || (grid[to] != EMPTY && grid[to] != FOOD && to != tail)) break;   /* a wall, or itself */
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
        if (ate) food();
    }
    sys_close(lcd);
    sys_close(in);
    printf("snake: length %d, score %d\n", length, score);
    return 0;
}
