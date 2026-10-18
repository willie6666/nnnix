/* SPDX-License-Identifier: GPL-3.0-or-later */
/* play: show an NNV picture or video (lib/nnv.h) on the LCD.
 *   play [-l] [-w] [-s] [-v] FILE
 *   -l  loop until Ctrl-C (also when the file has its loop flag)
 *   -w  wait for Enter after the last frame (always for a picture)
 *   -s  sound: play the file's sound on the buzzer (/dev/audio). Off unless
 *       asked for: the buzzer gets the rhythm across but little of the pitch
 *   -v  at the end, print the frame count, the time taken and late frames
 * The console text comes back when play ends. Make files with tools/mknnv.py.
 *
 * A program in /bin: its code runs from APROM, so playing takes about 2.3 KiB
 * of RAM, its task's stack and the state below, all in one block allocated
 * per run. */
#include "../user/cmds.h"
#include "../lib/stdio.h"
#include "../lib/string.h"
#include "../lib/sys.h"
#include "../lib/nnv.h"

#define AUDIO_RATE 8000u                 /* what /dev/audio plays */
#define CUT_SHORT (-2)                   /* fewer frames than the header says */

struct player {
    uint8_t frame[NNV_FRAME];
    uint8_t buf[512];                    /* 512: each fsd read, which waits its turn behind other jobs, brings more */
    uint32_t have, at;
    int fd, snd;
};

/* Make sure the read buffer holds something: 0, or -1 at the end of the file. */
static int fill(struct player *p)
{
    if (p->at < p->have) return 0;
    int r = sys_read(p->fd, p->buf, sizeof p->buf);
    if (r <= 0) return -1;
    p->have = (uint32_t)r;
    p->at = 0;
    return 0;
}

/* Up to n bytes of the file; fewer only at its end. */
static int get(struct player *p, uint8_t *dst, uint32_t n)
{
    uint32_t got = 0;
    while (got < n) {
        if (p->at == p->have) {
            int r = sys_read(p->fd, p->buf, sizeof p->buf);
            if (r <= 0) return got ? (int)got : r;
            p->have = (uint32_t)r;
            p->at = 0;
        }
        uint32_t k = p->have - p->at < n - got ? p->have - p->at : n - got;
        memcpy(dst + got, p->buf + p->at, k);
        p->at += k;
        got += k;
    }
    return (int)got;
}

/* Feed a delta record of len bytes straight from the read buffer. */
static int delta(struct player *p, uint32_t len)
{
    struct nnv_dec d;
    nnv_start(&d);
    while (len) {
        if (fill(p)) return -1;
        uint32_t k = p->have - p->at < len ? p->have - p->at : len;
        if (nnv_feed(&d, p->frame, p->buf + p->at, k)) return -1;
        p->at += k;
        len -= k;
    }
    return nnv_end(&d);
}

static int skip(struct player *p, uint32_t len)
{
    uint8_t scrap[16];
    while (len) {
        uint32_t k = len < sizeof scrap ? len : sizeof scrap;
        if (get(p, scrap, k) != (int)k) return -1;
        len -= k;
    }
    return 0;
}

/* Decode an audio record onto /dev/audio (or skip it when there is no sound). */
static int audio(struct player *p, uint32_t len)
{
    uint8_t h[3], pcm[64];
    struct nnv_adpcm a;
    if (len < 3 || get(p, h, 3) != 3 || nnv_adpcm_start(&a, h)) return -1;
    len -= 3;
    if (p->snd < 0) return skip(p, len);
    while (len) {
        if (fill(p)) return -1;
        uint32_t k = p->have - p->at < len ? p->have - p->at : len;
        if (k > sizeof pcm / 2) k = sizeof pcm / 2;
        nnv_adpcm_feed(&a, p->buf + p->at, k, pcm);
        sys_write(p->snd, pcm, 2 * k);
        p->at += k;
        len -= k;
    }
    return 0;
}

/* The next frame into `frame`: 1, 0 at the end of the file, -1 if damaged. */
static int next_frame(struct player *p)
{
    for (;;) {
        uint8_t rec[NNV_RECORD];
        int r = get(p, rec, NNV_RECORD);
        if (r == 0) return 0;
        if (r != NNV_RECORD) return -1;
        uint32_t len = nnv_u16(rec + 1);
        if (rec[0] == NNV_RAW) return len == NNV_FRAME && get(p, p->frame, NNV_FRAME) == NNV_FRAME ? 1 : -1;
        if (rec[0] == NNV_DELTA) return delta(p, len) ? -1 : 1;
        if (rec[0] == NNV_AUDIO ? audio(p, len) : skip(p, len)) return -1;
    }
}

static void wait_enter(void)
{
    char line[32];                                   /* the terminal hands over whole lines */
    if (sys_isatty(0)) (void)sys_read(0, line, sizeof line);
}

static int run(struct player *p, const char *path, int loop, int wait, int verbose, int sound)
{
    p->fd = sys_open(path, O_RDONLY);
    if (p->fd < 0) { printf("play: %s: %s\n", path, errstr(p->fd)); return 1; }
    uint8_t h[NNV_HEADER];
    struct nnv_info info;
    if (get(p, h, NNV_HEADER) != NNV_HEADER || nnv_header(h, &info)) {
        printf("play: %s: not an NNV file\n", path);
        return 1;
    }
    if (info.flags & NNV_LOOP) loop = 1;
    if (info.frames == 1) wait = 1;
    int lcd = sys_open("/dev/lcd", O_WRONLY);
    if (lcd < 0) { printf("play: /dev/lcd: %s\n", errstr(lcd)); return 1; }
    if (sound && !info.audio_rate) {
        printf("play: %s has no sound\n", path);
    } else if (sound && info.audio_rate != AUDIO_RATE) {
        printf("play: sound at %u Hz, /dev/audio plays %u Hz; playing without sound\n", info.audio_rate, AUDIO_RATE);
    } else if (sound) {
        p->snd = sys_open("/dev/audio", O_WRONLY);
        if (p->snd < 0) printf("play: /dev/audio: %s; playing without sound\n", errstr(p->snd));
    }

    /* Each frame is decoded first (queueing the sound that comes before it),
     * then shown at its time: sound written while waiting keeps playing. */
    int shown = 0, late = 0, pass = 0, r;             /* pass: frames since the start or a rewind */
    uint32_t start = sys_uptime(), due = start, frac = 0;
    for (;;) {
        r = next_frame(p);
        if (r == 0 && info.frames && pass != info.frames) r = CUT_SHORT;   /* the header knows how many */
        if (r == 0 && loop && shown) {               /* rewind: the first frame starts from blank */
            if (sys_lseek(p->fd, NNV_HEADER, SEEK_SET) < 0) { r = -1; break; }
            p->have = p->at = 0;
            pass = 0;
            memset(p->frame, 0, sizeof p->frame);
            continue;
        }
        if (r <= 0) break;
        int32_t early = (int32_t)(due - sys_uptime());
        if (early > 0) sys_sleep((uint32_t)early);
        else if (shown) {
            ++late;
            if (early < -500) due = sys_uptime();   /* far behind (slow drive): don't race to catch up */
        }
        sys_write(lcd, p->frame, NNV_FRAME);
        ++shown;
        ++pass;
        frac += info.frame_frac;                     /* 1/256 ms */
        due += info.frame_ms + (frac >> 8);
        frac &= 0xffu;
    }
    if (r == 0) {                                    /* the last frame gets its time too */
        int32_t left = (int32_t)(due - sys_uptime());
        if (left > 0) sys_sleep((uint32_t)left);
        if (wait) wait_enter();
    }
    uint32_t took = sys_uptime() - start;
    /* The sound first: it plays out what is queued (little by now), before the
     * LCD's text redraw could leave the device running dry. */
    if (p->snd >= 0) sys_close(p->snd);
    p->snd = -1;
    sys_close(lcd);
    if (verbose) printf("play: %d frames in %u.%03u s, %d late\n", shown, took / 1000u, took % 1000u, late);
    if (r == CUT_SHORT) { printf("play: %s: cut short: %d of %u frames\n", path, pass, info.frames); return 1; }
    if (r < 0) { printf("play: %s: damaged after %d frames\n", path, shown); return 1; }
    return 0;
}

int cmd_play(int argc, char **argv)
{
    int loop = 0, wait = 0, verbose = 0, sound = 0;
    const char *path = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "-l")) loop = 1;
        else if (!strcmp(argv[i], "-w")) wait = 1;
        else if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (!strcmp(argv[i], "-s")) sound = 1;
        else if (argv[i][0] != '-' && !path) path = argv[i];
        else path = 0, argc = 0;
    }
    if (!path) { printf("usage: play [-l] [-w] [-s] [-v] FILE\n"); return 2; }
    struct player *p = sys_alloc(sizeof *p);
    if (!p) { printf("play: %s\n", errstr(SYS_ENOMEM)); return 1; }
    memset(p, 0, sizeof *p);
    p->fd = p->snd = -1;
    int rc = run(p, path, loop, wait, verbose, sound);
    if (p->snd >= 0) sys_close(p->snd);
    if (p->fd >= 0) sys_close(p->fd);
    sys_free(p);
    return rc;
}
