/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The games' shared part (game.h). The picture is in /dev/lcd's layout: byte
 * x + 128 * (y / 8), bit y % 8; a set bit is dark. */
#include "game.h"
#include "../lib/fmt.h"
#include "../lib/nnlib.h"
#include "../lib/stdio.h"
#include "../lib/string.h"
#include "../lib/sys.h"
#include "../user/cmds.h"

extern const struct nn_lib *__nn_lib;    /* sdk/crt0.c: the fonts are the firmware's */

static uint8_t fb[GAME_W * GAME_H / 8];
static int in = -1, lcd = -1;
static unsigned held, pressed;
static uint32_t seed;

int game_open(const char *name)
{
    if (__nn_lib->version < 2) { printf("%s: the firmware is too old (no fonts)\n", name); return 1; }
    in = sys_open("/dev/input", O_RDONLY | O_NONBLOCK);
    if (in < 0) { printf("%s: /dev/input: %s\n", name, errstr(in)); return 1; }
    lcd = sys_open("/dev/lcd", O_WRONLY);
    if (lcd < 0) { printf("%s: /dev/lcd: %s\n", name, errstr(lcd)); sys_close(in); return 1; }
    seed = sys_uptime() * 2654435761u + 1u;
    held = pressed = 0;
    return 0;
}

void game_close(void)
{
    sys_close(lcd);
    sys_close(in);
}

static unsigned action(const struct nn_input_event *e)
{
    if (e->type == NN_INPUT_PAD) {
        static const uint8_t pad[10] = { 0, GAME_QUIT, GAME_UP, 0, GAME_LEFT, GAME_A, GAME_RIGHT, 0, GAME_DOWN, 0 };
        return e->code < 10 ? pad[e->code] : 0;
    }
    if (e->type != NN_INPUT_KEY) return 0;
    switch (e->code) {
    case NN_KEY_UP: case NN_KEY_A + 'w' - 'a': return GAME_UP;
    case NN_KEY_DOWN: case NN_KEY_A + 's' - 'a': return GAME_DOWN;
    case NN_KEY_LEFT: case NN_KEY_A: return GAME_LEFT;
    case NN_KEY_RIGHT: case NN_KEY_A + 'd' - 'a': return GAME_RIGHT;
    case NN_KEY_SPACE: case NN_KEY_ENTER: return GAME_A;
    case NN_KEY_ESC: case NN_KEY_A + 'q' - 'a': return GAME_QUIT;
    default: return 0;
    }
}

void game_poll(void)
{
    struct nn_input_event ev[4];
    int n;
    while ((n = sys_read(in, ev, sizeof ev)) > 0)
        for (int i = 0; i < n / (int)sizeof ev[0]; ++i) {
            unsigned a = action(&ev[i]);
            if (ev[i].value) { held |= a; pressed |= a; }
            else held &= ~a;
        }
}

unsigned game_pressed(void)
{
    game_poll();
    unsigned p = pressed;
    pressed = 0;
    return p;
}

unsigned game_held(void) { return held; }

void game_show_until(uint32_t at)
{
    sys_write(lcd, fb, sizeof fb);
    game_wait_until(at);
}

void game_wait_until(uint32_t at)
{
    for (;;) {
        game_poll();
        int32_t left = (int32_t)(at - sys_uptime());
        if (left <= 0) return;
        sys_sleep(left < 10 ? (uint32_t)left : 10u);
    }
}

void game_clear(void) { memset(fb, 0, sizeof fb); }

void game_pixel(int x, int y, int on)
{
    if (x < 0 || x >= GAME_W || y < 0 || y >= GAME_H) return;
    uint8_t *b = &fb[x + GAME_W * (y >> 3)], bit = (uint8_t)(1u << (y & 7));
    *b = on ? (uint8_t)(*b | bit) : (uint8_t)(*b & ~bit);
}

int game_get(int x, int y)
{
    if (x < 0 || x >= GAME_W || y < 0 || y >= GAME_H) return 1;      /* outside counts as a wall */
    return fb[x + GAME_W * (y >> 3)] >> (y & 7) & 1;
}

void game_fill(int x, int y, int w, int h, int on)
{
    for (int j = 0; j < h; ++j)
        for (int i = 0; i < w; ++i) game_pixel(x + i, y + j, on);
}

void game_box(int x, int y, int w, int h)
{
    game_fill(x, y, w, 1, 1);
    game_fill(x, y + h - 1, w, 1, 1);
    game_fill(x, y, 1, h, 1);
    game_fill(x + w - 1, y, 1, h, 1);
}

static int draw(int x, int y, const char *s, int big)
{
    int step = big ? 6 : 4, width = (int)strlen(s) * step - 1;
    if (x < 0) x = (GAME_W - width) / 2;
    for (; *s; ++s, x += step) {
        unsigned c = (unsigned char)*s < 0x20 || (unsigned char)*s > 0x7f ? '?' - 0x20 : (unsigned char)*s - 0x20u;
        for (int col = 0; col < step - 1; ++col)
            for (int row = 0; row < (big ? 7 : 5); ++row) {
                int on = big ? __nn_lib->font5x7[c][col] >> row & 1 : __nn_lib->font3x5[c] >> (col * 5 + row) & 1;
                game_pixel(x + col, y + row, on);
            }
    }
    return width;
}

int game_text(int x, int y, const char *s) { return draw(x, y, s, 0); }
int game_big(int x, int y, const char *s) { return draw(x, y, s, 1); }

int game_number(int x, int y, unsigned n)
{
    char digits[12];
    nn_snprintf(digits, sizeof digits, "%u", n);
    return game_text(x, y, digits);
}

uint32_t game_rand(void)
{
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}

int game_over(unsigned score, unsigned *best)
{
    char line[24];
    if (score > *best) *best = score;
    game_fill(24, 14, 80, 36, 0);
    game_box(24, 14, 80, 36);
    game_big(-1, 18, "GAME OVER");
    nn_snprintf(line, sizeof line, "SCORE %u", score);
    game_text(-1, 29, line);
    nn_snprintf(line, sizeof line, "BEST %u", *best);
    game_text(-1, 36, line);
    game_text(-1, 43, "A: AGAIN  Q: QUIT");
    game_show_until(sys_uptime() + 400u);            /* a moment before a key counts */
    game_pressed();
    for (;;) {
        unsigned p = game_pressed();
        if (p & GAME_QUIT) return 0;
        if (p & GAME_A) return 1;
        sys_sleep(20);
    }
}
