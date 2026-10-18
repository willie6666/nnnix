/* SPDX-License-Identifier: GPL-3.0-or-later */
/* A game of bin/ on the host: its system calls answered here on a clock of
 * its own (sleeps take no real time), the keys pressed from a script, and the
 * LCD saved as PBM pictures when the script asks.
 *
 *   game_sim [-o DIR] STEP...       (built per game: build/host/sim_tetris ...)
 *   STEP: MS:KEY    press KEY at MS ms, release it 60 ms later
 *         MS:+KEY   press only        MS:-KEY  release
 *         MS:shot   save the screen as DIR/MS.pbm (DIR: the current one)
 *   KEY: up down left right space enter esc q w a s d, pad1 ... pad9
 * The game's output goes to stdout, then "sim: ended at MS ms". It must end
 * by the script's last step + 60 s (a script that does not end it fails). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../lib/nnlib.h"
#include "../../lib/sys.h"
#include "../../drivers/font3x5.h"
#include "../../drivers/font5x7.h"

int GAME(int argc, char **argv);

static const struct nn_lib lib = { .magic = NN_LIB_MAGIC, .version = NN_LIB_VERSION, .size = sizeof lib,
                                   .font3x5 = font3x5, .font5x7 = font5x7 };
const struct nn_lib *__nn_lib = &lib;
const char *errstr(int e) { (void)e; return "error"; }

#define FD_INPUT 10
#define FD_LCD 11
#define STEPS 512

static struct step { uint32_t at; uint8_t type, code, down, shot; } steps[STEPS];
static int nsteps, next_step;
static uint32_t now, limit;
static uint8_t frame[1024];
static const char *dir = ".";

static void shot(uint32_t at)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%06u.pbm", dir, (unsigned)at);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(2); }
    fprintf(f, "P1\n128 64\n");
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 128; ++x) fputc('0' + (frame[x + 128 * (y / 8)] >> (y % 8) & 1), f);
        fputc('\n', f);
    }
    fclose(f);
}

/* Steps that are due: screenshots now (the picture last shown); key events
 * are taken by reads. */
static void advance(uint32_t ms)
{
    now += ms ? ms : 1u;
    while (next_step < nsteps && steps[next_step].shot && steps[next_step].at <= now) shot(steps[next_step++].at);
    if (now > limit) { printf("sim: still running at %u ms\n", (unsigned)now); exit(1); }
}

intptr_t host_syscall(int n, intptr_t a, intptr_t b, intptr_t c)
{
    switch (n) {
    case SYS_UPTIME: return (intptr_t)now;
    case SYS_SLEEP: advance((uint32_t)a); return 0;
    case SYS_OPEN:
        if (!strcmp((const char *)a, "/dev/input")) return FD_INPUT;
        if (!strcmp((const char *)a, "/dev/lcd")) return FD_LCD;
        return SYS_ENOENT;
    case SYS_CLOSE: return 0;
    case SYS_WRITE:
        if (a == FD_LCD) { memcpy(frame, (const void *)b, c < 1024 ? (size_t)c : 1024u); return c; }
        fwrite((const void *)b, 1, (size_t)c, stdout);
        return c;
    case SYS_READ: {
        if (a != FD_INPUT) return 0;
        struct nn_input_event *e = (struct nn_input_event *)b;
        int got = 0;
        while (got < c / 8 && next_step < nsteps && steps[next_step].at <= now) {
            struct step *s = &steps[next_step++];
            if (s->shot) { shot(s->at); continue; }
            e[got++] = (struct nn_input_event){ s->type, s->code, s->down, 0, 0, 0, (uint16_t)s->at };
        }
        return got ? got * 8 : SYS_EAGAIN;
    }
    default:
        printf("sim: system call %d not simulated\n", n);
        exit(2);
    }
}

static int add(uint32_t at, int type, int code, int down, int is_shot)
{
    if (nsteps == STEPS) return -1;
    int i = nsteps++;
    while (i > 0 && steps[i - 1].at > at) { steps[i] = steps[i - 1]; --i; }   /* in time order, stable */
    steps[i] = (struct step){ at, (uint8_t)type, (uint8_t)code, (uint8_t)down, (uint8_t)is_shot };
    return 0;
}

static int key(const char *name, int *type, int *code)
{
    static const struct { const char *name; int code; } keys[] = {
        { "up", NN_KEY_UP }, { "down", NN_KEY_DOWN }, { "left", NN_KEY_LEFT }, { "right", NN_KEY_RIGHT },
        { "space", NN_KEY_SPACE }, { "enter", NN_KEY_ENTER }, { "esc", NN_KEY_ESC },
        { "q", NN_KEY_A + 'q' - 'a' }, { "w", NN_KEY_A + 'w' - 'a' }, { "a", NN_KEY_A },
        { "s", NN_KEY_A + 's' - 'a' }, { "d", NN_KEY_A + 'd' - 'a' },
    };
    if (!strncmp(name, "pad", 3) && name[3] >= '1' && name[3] <= '9' && !name[4]) {
        *type = NN_INPUT_PAD;
        *code = name[3] - '0';
        return 0;
    }
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; ++i)
        if (!strcmp(name, keys[i].name)) { *type = NN_INPUT_KEY; *code = keys[i].code; return 0; }
    return -1;
}

int main(int argc, char **argv)
{
    int i = 1;
    if (argc > 2 && !strcmp(argv[1], "-o")) { dir = argv[2]; i = 3; }
    for (; i < argc; ++i) {
        char *colon = strchr(argv[i], ':'), *name = colon ? colon + 1 : 0;
        int type, code, ok = colon != 0;
        uint32_t at = (uint32_t)strtoul(argv[i], 0, 10);
        if (ok && !strcmp(name, "shot")) ok = !add(at, 0, 0, 0, 1);
        else if (ok && (*name == '+' || *name == '-')) ok = !key(name + 1, &type, &code) && !add(at, type, code, *name == '+', 0);
        else if (ok) ok = !key(name, &type, &code) && !add(at, type, code, 1, 0) && !add(at + 60, type, code, 0, 0);
        if (!ok) { fprintf(stderr, "game_sim: bad step %s\n", argv[i]); return 2; }
        if (at + 60000u > limit) limit = at + 60000u;
    }
    if (!limit) limit = 60000u;
    char *args[] = { "game", 0 };
    int rc = GAME(1, args);
    printf("sim: ended at %u ms\n", (unsigned)now);
    return rc;
}
