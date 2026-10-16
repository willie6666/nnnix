/* SPDX-License-Identifier: GPL-3.0-or-later */
/* play: show an NNV picture or video (lib/nnv.h) on the LCD.
 *   play [-l] [-w] FILE
 *   -l  loop until Ctrl-C (also when the file has its loop flag)
 *   -w  wait for Enter after the last frame (always for a picture)
 * The console text comes back when play ends. Make files with tools/mknnv.py. */
#include "nnnix.h"
#include "nnv.h"

static uint8_t frame[NNV_FRAME];
static uint8_t buf[128];
static uint32_t have, at;
static int fd;

/* Up to n bytes of the file; fewer only at its end. */
static int get(uint8_t *dst, uint32_t n)
{
    uint32_t got = 0;
    while (got < n) {
        if (at == have) {
            int r = sys_read(fd, buf, sizeof buf);
            if (r <= 0) return got ? (int)got : r;
            have = (uint32_t)r;
            at = 0;
        }
        uint32_t k = have - at < n - got ? have - at : n - got;
        memcpy(dst + got, buf + at, k);
        at += k;
        got += k;
    }
    return (int)got;
}

/* Feed a delta record of len bytes straight from the read buffer. */
static int delta(uint32_t len)
{
    struct nnv_dec d;
    nnv_start(&d);
    while (len) {
        if (at == have) {
            int r = sys_read(fd, buf, sizeof buf);
            if (r <= 0) return -1;
            have = (uint32_t)r;
            at = 0;
        }
        uint32_t k = have - at < len ? have - at : len;
        if (nnv_feed(&d, frame, buf + at, k)) return -1;
        at += k;
        len -= k;
    }
    return nnv_end(&d);
}

static int skip(uint32_t len)
{
    uint8_t scrap[16];
    while (len) {
        uint32_t k = len < sizeof scrap ? len : sizeof scrap;
        if (get(scrap, k) != (int)k) return -1;
        len -= k;
    }
    return 0;
}

/* The next frame into `frame`: 1, 0 at the end of the file, -1 if damaged. */
static int next_frame(void)
{
    for (;;) {
        uint8_t rec[NNV_RECORD];
        int r = get(rec, NNV_RECORD);
        if (r == 0) return 0;
        if (r != NNV_RECORD) return -1;
        uint32_t len = nnv_u16(rec + 1);
        if (rec[0] == NNV_RAW) return len == NNV_FRAME && get(frame, NNV_FRAME) == NNV_FRAME ? 1 : -1;
        if (rec[0] == NNV_DELTA) return delta(len) ? -1 : 1;
        if (skip(len)) return -1;
    }
}

static void wait_enter(void)
{
    char line[32];                                   /* the terminal hands over whole lines */
    if (sys_isatty(0)) (void)sys_read(0, line, sizeof line);
}

int main(int argc, char **argv)
{
    int loop = 0, wait = 0;
    const char *path = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "-l")) loop = 1;
        else if (!strcmp(argv[i], "-w")) wait = 1;
        else if (argv[i][0] != '-' && !path) path = argv[i];
        else path = 0, argc = 0;
    }
    if (!path) { puts("usage: play [-l] [-w] FILE"); return 2; }
    fd = sys_open(path, O_RDONLY);
    if (fd < 0) { printf("play: %s: %s\n", path, errstr(fd)); return 1; }
    uint8_t h[NNV_HEADER];
    struct nnv_info info;
    if (get(h, NNV_HEADER) != NNV_HEADER || nnv_header(h, &info)) {
        printf("play: %s: not an NNV file\n", path);
        return 1;
    }
    if (info.flags & NNV_LOOP) loop = 1;
    if (info.frames == 1) wait = 1;
    int lcd = sys_open("/dev/lcd", O_WRONLY);
    if (lcd < 0) { printf("play: /dev/lcd: %s\n", errstr(lcd)); return 1; }

    int shown = 0, r;
    uint32_t due = sys_uptime();
    for (;;) {
        r = next_frame();
        if (r == 0 && loop && shown) {               /* rewind: the first frame starts from blank */
            if (sys_lseek(fd, NNV_HEADER, SEEK_SET) < 0) { r = -1; break; }
            have = at = 0;
            memset(frame, 0, sizeof frame);
            continue;
        }
        if (r <= 0) break;
        sys_write(lcd, frame, NNV_FRAME);
        ++shown;
        due += info.frame_ms;
        int32_t early = (int32_t)(due - sys_uptime());
        if (early > 0) sys_sleep((uint32_t)early);
        else if (early < -500) due = sys_uptime();  /* far behind (slow drive): don't race to catch up */
    }
    if (r == 0 && wait) wait_enter();
    sys_close(lcd);
    if (r < 0) { printf("play: %s: damaged after %d frames\n", path, shown); return 1; }
    return 0;
}
