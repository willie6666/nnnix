/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NNNIX_NNV_H
#define NNNIX_NNV_H
#include <stdint.h>

/* NNV: pictures and videos for the onboard 128x64 LCD (docs/NNV.md).
 *
 * A file is a 16-byte header and then frame records. A picture is a video of
 * one frame. All numbers are little-endian.
 *
 *   header  "NNV1", u16 width (128), u16 height (64), u16 frames (0: unknown),
 *           u16 frame_ms (time per frame), u8 flags (NNV_LOOP),
 *           u8 frame_frac (and frame_frac/256 ms more: 30 fps is 33 + 85/256),
 *           2 bytes zero
 *   record  u8 type, u16 length, length bytes:
 *           NNV_RAW    the 1024-byte frame itself
 *           NNV_DELTA  codes that turn the previous frame into this one (the
 *                      frame before the first, and after a rewind, is blank)
 *           decoders skip records of other types
 *
 * A frame is laid out like the LCD (drivers/lcd.h): byte i holds x = i % 128
 * of the 8-row band i / 128, bit 0 the top row of the band; a set bit is dark.
 *
 * Delta codes, each followed by its operands, and together never past the end
 * of the frame:
 *   0x00-0x7f  c+1 literal bytes follow
 *   0x80-0xbf  skip (c&0x3f)+1 bytes: they stay as in the previous frame
 *   0xc0-0xff  one byte follows, repeated (c&0x3f)+1 times
 *
 * nnv_dec decodes a delta record a piece at a time, so a player needs only
 * the frame and a small read buffer. Shared by sdk/examples/play.c and the
 * host test against tools/mknnv.py. */

#define NNV_WIDTH 128u
#define NNV_HEIGHT 64u
#define NNV_FRAME 1024u
#define NNV_HEADER 16u
#define NNV_RECORD 3u                /* type and length */

enum { NNV_RAW = 0, NNV_DELTA = 1 };
#define NNV_LOOP 0x01u

struct nnv_info { uint16_t frames, frame_ms; uint8_t flags, frame_frac; };

static inline uint16_t nnv_u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* 0, or -1 when this is not an NNV file this decoder understands. */
static inline int nnv_header(const uint8_t h[NNV_HEADER], struct nnv_info *info)
{
    if (h[0] != 'N' || h[1] != 'N' || h[2] != 'V' || h[3] != '1') return -1;
    if (nnv_u16(h + 4) != NNV_WIDTH || nnv_u16(h + 6) != NNV_HEIGHT) return -1;
    info->frames = nnv_u16(h + 8);
    info->frame_ms = nnv_u16(h + 10);
    info->flags = h[12];
    info->frame_frac = h[13];
    return 0;
}

struct nnv_dec { uint16_t pos; uint8_t op, left; };
enum { NNV_CODE, NNV_LITERAL, NNV_FILL };

static inline void nnv_start(struct nnv_dec *d) { d->pos = 0; d->op = NNV_CODE; d->left = 0; }

/* Apply n more bytes of a delta record to frame. -1 on a code that runs past
 * the end of the frame. */
static inline int nnv_feed(struct nnv_dec *d, uint8_t *frame, const uint8_t *p, uint32_t n)
{
    for (uint32_t i = 0; i < n; ++i) {
        uint8_t b = p[i];
        if (d->op == NNV_LITERAL) {
            frame[d->pos++] = b;
            if (!--d->left) d->op = NNV_CODE;
        } else if (d->op == NNV_FILL) {
            for (; d->left; --d->left) frame[d->pos++] = b;
            d->op = NNV_CODE;
        } else {
            uint32_t count = b < 0x80u ? b + 1u : (b & 0x3fu) + 1u;
            if (d->pos + count > NNV_FRAME) return -1;
            if (b < 0x80u) { d->op = NNV_LITERAL; d->left = (uint8_t)count; }
            else if (b < 0xc0u) d->pos = (uint16_t)(d->pos + count);
            else { d->op = NNV_FILL; d->left = (uint8_t)count; }
        }
    }
    return 0;
}

/* The record ended: 0 if it ended between codes. */
static inline int nnv_end(const struct nnv_dec *d) { return d->op == NNV_CODE ? 0 : -1; }

#endif
