/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The NNV decoder (lib/nnv.h) against files made by tools/mknnv.py (the
 * Makefile generates them with the frames they hold) and hand-made codes. */
#include "../../lib/nnv.h"
#include "check.h"
#include <stdlib.h>
#include <string.h>

static uint8_t *load(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END);
    *size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *p = malloc(*size ? *size : 1);
    if (fread(p, 1, *size, f) != *size) exit(1);
    fclose(f);
    return p;
}

/* Decode a whole file, feeding delta records `piece` bytes at a time, the way
 * play.c does from its read buffer; compare with the reference frames. */
static void check_file(const char *name, uint32_t piece)
{
    char path[128];
    size_t size, raw_size;
    snprintf(path, sizeof path, "../../build/host/nnv_%s.nnv", name);
    uint8_t *data = load(path, &size);
    snprintf(path, sizeof path, "../../build/host/nnv_%s.raw", name);
    uint8_t *raw = load(path, &raw_size);
    struct nnv_info info;
    CHECK(size >= NNV_HEADER && nnv_header(data, &info) == 0);
    CHECK_EQ_INT(info.frames * NNV_FRAME, raw_size);
    uint8_t frame[NNV_FRAME] = { 0 };
    size_t at = NNV_HEADER, n = 0;
    int bad = 0;
    while (at + NNV_RECORD <= size) {
        uint32_t len = nnv_u16(data + at + 1);
        const uint8_t *body = data + at + NNV_RECORD;
        CHECK(at + NNV_RECORD + len <= size);
        if (data[at] == NNV_RAW) {
            CHECK_EQ_INT(len, NNV_FRAME);
            memcpy(frame, body, NNV_FRAME);
        } else {
            CHECK_EQ_INT(data[at], NNV_DELTA);
            struct nnv_dec d;
            nnv_start(&d);
            for (uint32_t k = 0; k < len; k += piece)
                if (nnv_feed(&d, frame, body + k, len - k < piece ? len - k : piece)) bad++;
            if (nnv_end(&d)) bad++;
        }
        if (n < info.frames && memcmp(frame, raw + n * NNV_FRAME, NNV_FRAME)) bad++;
        ++n;
        at += NNV_RECORD + len;
    }
    CHECK_EQ_INT(bad, 0);
    CHECK_EQ_INT(at, size);
    CHECK_EQ_INT(n, info.frames);
    free(data);
    free(raw);
}

static int run(uint8_t *frame, const uint8_t *codes, uint32_t n)
{
    struct nnv_dec d;
    nnv_start(&d);
    if (nnv_feed(&d, frame, codes, n)) return -1;
    return nnv_end(&d);
}

static void check_codes(void)
{
    uint8_t f[NNV_FRAME];
    uint8_t c[1100];

    /* Each kind once: 3 literal bytes, skip 4, fill 5 with 0xaa. */
    memset(f, 0x11, sizeof f);
    const uint8_t mix[] = { 0x02, 1, 2, 3, 0x83, 0xc4, 0xaa };
    CHECK_EQ_INT(run(f, mix, sizeof mix), 0);
    CHECK(f[0] == 1 && f[1] == 2 && f[2] == 3);
    CHECK(f[3] == 0x11 && f[6] == 0x11);
    CHECK(f[7] == 0xaa && f[11] == 0xaa && f[12] == 0x11);

    /* Longest of each: literal 128, skip 64, fill 64. */
    memset(f, 0, sizeof f);
    c[0] = 0x7f;
    for (int i = 0; i < 128; ++i) c[1 + i] = (uint8_t)(i + 1);
    c[129] = 0xbf;
    c[130] = 0xff;
    c[131] = 0x5a;
    CHECK_EQ_INT(run(f, c, 132), 0);
    CHECK(f[0] == 1 && f[127] == 128 && f[128] == 0 && f[191] == 0);
    CHECK(f[192] == 0x5a && f[255] == 0x5a && f[256] == 0);

    /* A whole frame of fills ends exactly at the end; one more byte does not fit. */
    for (int i = 0; i < 16; ++i) { c[2 * i] = 0xff; c[2 * i + 1] = 0xee; }
    CHECK_EQ_INT(run(f, c, 32), 0);
    CHECK(f[0] == 0xee && f[1023] == 0xee);
    c[32] = 0x00;
    c[33] = 0x01;
    CHECK_EQ_INT(run(f, c, 34), -1);

    /* Codes that would run past the end are refused before writing. */
    memset(f, 0, sizeof f);
    for (int i = 0; i < 15; ++i) c[i] = 0xbf;        /* skip 960 */
    c[15] = 0xbf;                                     /* 1024: still fits */
    c[16] = 0x80;                                     /* 1025 */
    CHECK_EQ_INT(run(f, c, 16), 0);
    CHECK_EQ_INT(run(f, c, 17), -1);
    for (int i = 0; i < 15; ++i) c[i] = 0xbf;
    c[15] = 0x7f;                                     /* 128 literal bytes at 960 */
    CHECK_EQ_INT(run(f, c, 16), -1);
    c[15] = 0xff;                                     /* fill 64 at 960: fits */
    c[16] = 0x77;
    CHECK_EQ_INT(run(f, c, 17), 0);
    CHECK(f[1023] == 0x77);

    /* Records that end inside a code. */
    const uint8_t cut_lit[] = { 0x03, 1, 2 }, cut_fill[] = { 0xc0 };
    CHECK_EQ_INT(run(f, cut_lit, sizeof cut_lit), -1);
    CHECK_EQ_INT(run(f, cut_fill, sizeof cut_fill), -1);
    CHECK_EQ_INT(run(f, c, 0), 0);                    /* empty: the same frame again */

    /* Headers. */
    uint8_t h[NNV_HEADER] = { 'N', 'N', 'V', '1', 128, 0, 64, 0, 3, 0, 33, 0, NNV_LOOP, 85 };
    struct nnv_info info;
    CHECK_EQ_INT(nnv_header(h, &info), 0);
    CHECK(info.frames == 3 && info.frame_ms == 33 && info.frame_frac == 85 && info.flags == NNV_LOOP);
    h[6] = 32;
    CHECK_EQ_INT(nnv_header(h, &info), -1);
    h[6] = 64;
    h[3] = '2';
    CHECK_EQ_INT(nnv_header(h, &info), -1);
}

int main(void)
{
    static const char *const files[] = { "pattern", "bounce", "mixed" };
    static const uint32_t pieces[] = { 1, 7, 128, 65536 };
    check_codes();
    for (unsigned i = 0; i < sizeof files / sizeof files[0]; ++i)
        for (unsigned p = 0; p < sizeof pieces / sizeof pieces[0]; ++p) check_file(files[i], pieces[p]);
    return check_summary("test_nnv");
}
