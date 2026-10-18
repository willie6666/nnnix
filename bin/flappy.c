/* SPDX-License-Identifier: GPL-3.0-or-later */
/* flappy: A or up flaps; fly through the gaps between the pipes. Keys:
 * bin/game.h. */
#include "game.h"
#include "../lib/fmt.h"
#include "../lib/stdio.h"
#include "../lib/sys.h"

#define FRAME_MS 30u
#define BIRD_X 24
#define GROUND 61                     /* the first row of the ground */
#define PIPE_W 10
#define PIPES 4
#define SPACING 48                    /* pixels from one pipe to the next */
/* The bird's height in 1/16 pixel. */
#define GRAVITY 5
#define FLAP (-52)
#define FALL_MAX 64

static const uint8_t bird[4] = { 0x0e, 0x1b, 0x1f, 0x0e };   /* rows, bit 0 the left column */
static int quit;

struct pipe { int x, gap, h; uint8_t passed; };

static void draw_bird(int y)
{
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 5; ++c)
            if (bird[r] >> c & 1u) game_pixel(BIRD_X + c, y + r, 1);
}

/* The bird's pixels over anything already drawn (the pipes, the ground). */
static int hits(int y)
{
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 5; ++c)
            if (bird[r] >> c & 1u && game_get(BIRD_X + c, y + r)) return 1;
    return 0;
}

static void scene(const struct pipe *p, unsigned score, uint32_t frame)
{
    char digits[12];
    game_clear();
    for (int i = 0; i < PIPES; ++i) {
        game_fill(p[i].x, 0, PIPE_W, p[i].gap, 1);
        game_fill(p[i].x - 1, p[i].gap - 3, PIPE_W + 2, 3, 1);          /* the pipes' lips */
        game_fill(p[i].x - 1, p[i].gap + p[i].h, PIPE_W + 2, 3, 1);
        game_fill(p[i].x, p[i].gap + p[i].h, PIPE_W, GROUND - p[i].gap - p[i].h, 1);
    }
    game_fill(0, GROUND, GAME_W, 1, 1);
    for (int x = 0; x < GAME_W; x += 4) game_pixel(x + 3 - (int)(frame % 4u), GROUND + 2, 1);   /* the ground moves too */
    nn_snprintf(digits, sizeof digits, "%u", score);
    game_fill(GAME_W / 2 - 10, 1, 21, 9, 0);
    game_big(-1, 2, digits);
}

static void new_pipe(struct pipe *p, int x, unsigned score)
{
    p->h = score < 20 ? 24 - (int)score / 4 : 19;           /* the gap narrows a little */
    p->gap = 6 + (int)(game_rand() % (unsigned)(GROUND - 12 - p->h));
    p->x = x;
    p->passed = 0;
}

static unsigned play(void)
{
    struct pipe p[PIPES];
    int y16 = 28 * 16, vy = 0;
    unsigned score = 0;
    for (int i = 0; i < PIPES; ++i) new_pipe(&p[i], GAME_W + 40 + i * SPACING, 0);
    /* Hover until the first flap. */
    game_pressed();
    for (uint32_t now = sys_uptime(), t = 0;; ++t) {
        game_clear();
        game_big(-1, 8, "FLAPPY");
        game_text(-1, 40, "A OR UP: FLY");
        game_fill(0, GROUND, GAME_W, 1, 1);
        draw_bird(28 + (int)(t / 8u % 2u));
        game_show_until(now += FRAME_MS);
        unsigned pressed = game_pressed();
        if (pressed & GAME_QUIT) { quit = 1; return 0; }
        if (pressed & (GAME_A | GAME_UP)) { vy = FLAP; break; }
    }
    for (uint32_t now = sys_uptime(), frame = 0;; ++frame) {
        unsigned pressed = game_pressed();
        if (pressed & GAME_QUIT) { quit = 1; return score; }
        if (pressed & (GAME_A | GAME_UP)) vy = FLAP;
        vy = vy + GRAVITY > FALL_MAX ? FALL_MAX : vy + GRAVITY;
        y16 += vy;
        if (y16 < 0) { y16 = 0; vy = 0; }                /* the top of the screen holds it */
        for (int i = 0; i < PIPES; ++i) {
            p[i].x--;
            if (!p[i].passed && p[i].x + PIPE_W < BIRD_X) { p[i].passed = 1; ++score; }
            if (p[i].x + PIPE_W + 1 < 0) new_pipe(&p[i], p[i].x + PIPES * SPACING, score);
        }
        scene(p, score, frame);
        int y = y16 / 16;
        int dead = hits(y);
        draw_bird(y);
        game_show_until(now += FRAME_MS);
        if (dead) return score;
    }
}

int cmd_flappy(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (game_open("flappy")) return 1;
    unsigned best = 0, score;
    do score = play(); while (!quit && game_over(score, &best));
    if (score > best) best = score;
    game_close();
    printf("flappy: best %u\n", best);
    return 0;
}
