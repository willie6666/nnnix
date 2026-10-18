/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NNNIX_AUDIORING_H
#define NNNIX_AUDIORING_H
#include <stdint.h>
#ifdef NNNIX_HOST
#include <string.h>
#else
#include "../lib/string.h"
#endif
#include "../lib/nnv.h"

/* The ring between the writers of /dev/audio and /dev/adpcm and the 8 kHz
 * timer interrupt (drivers/audio.c), apart from the hardware so the host test
 * (tests/host/test_audio.c) runs it as it is.
 *
 * PCM (/dev/audio): the ring holds the samples themselves.
 *
 * ADPCM (/dev/adpcm): writes are NNV sound records back to back, as they are
 * in a file (lib/nnv.h: type 2, u16 length, i16 predictor, u8 step index,
 * codes), and the interrupt decodes them, so a ring holds twice as much
 * sound (4-bit codes instead of 8-bit samples). The ring holds blocks of
 * [n, predictor low, high, index] and n code bytes (n 1-255); a record with
 * more codes goes on in blocks whose index is AR_KEEP, which keep the
 * decoder's state. A block's 4 bytes are queued
 * together with its first codes, the rest as they come: the interrupt plays
 * what has arrived.
 *
 * One task writes (ar_put), the interrupt reads (ar_next): head moves only in
 * ar_put, tail only in ar_next. */

#define AR_KEEP 0xffu                /* a block that goes on with the decoder's state */
#define AR_BLOCK 4u                  /* a block's bytes before its codes */

struct audio_ring {
    uint8_t *buf;
    uint32_t size;                   /* a power of two */
    volatile uint32_t head, tail;
    uint8_t adpcm;
    /* the writer's: the record header being gathered, then its codes */
    uint8_t rec[6], got, fresh, bad;
    uint16_t rec_left;               /* codes of this record not yet queued */
    uint8_t blk_left;                /* of them, in the block queued last */
    /* the interrupt's */
    uint8_t left, code, high;        /* codes left in the block; the byte being played; its high nibble next */
    struct nnv_adpcm dec;
};

static inline void ar_init(struct audio_ring *r, uint8_t *buf, uint32_t size, int adpcm)
{
    memset(r, 0, sizeof *r);
    r->buf = buf;
    r->size = size;
    r->adpcm = (uint8_t)adpcm;
}

static inline uint32_t ar_used(const struct audio_ring *r) { return r->head - r->tail; }

/* Whether the interrupt has played all that was queued (a block's codes still
 * to come are not waited for). */
static inline int ar_empty(const struct audio_ring *r) { return r->head == r->tail && !r->high; }

static inline void ar_copy(struct audio_ring *r, uint32_t h, const uint8_t *p, uint32_t k)
{
    uint32_t at = h & (r->size - 1u), first = r->size - at < k ? r->size - at : k;
    memcpy(r->buf + at, p, first);
    memcpy(r->buf, p + first, k - first);
}

/* Queue what fits of n bytes: returns how many were taken (fewer than n when
 * the ring is full). ADPCM: a record header that is not a sound record (or
 * has a step index above 88) sets bad and stops; the caller fails the write. */
static inline uint32_t ar_put(struct audio_ring *r, const uint8_t *p, uint32_t n)
{
    uint32_t done = 0;
    while (done < n) {
        uint32_t h = r->head, room = r->size - (h - r->tail), k = n - done;
        if (r->adpcm && !r->blk_left) {
            if (!r->rec_left) {                      /* gather the next record's header */
                r->rec[r->got++] = p[done++];
                if (r->got < sizeof r->rec) continue;
                r->got = 0;
                uint32_t len = nnv_u16(r->rec + 1);
                if (r->rec[0] != NNV_AUDIO || len < 3 || r->rec[5] > 88) { r->bad = 1; break; }
                r->rec_left = (uint16_t)(len - 3);
                r->fresh = 1;
                continue;
            }
            /* A block, queued with its first codes (some are here: done < n),
             * so the interrupt never finds one without. */
            if (room < AR_BLOCK + 1u) break;
            uint8_t b[AR_BLOCK] = { r->rec_left < 255u ? (uint8_t)r->rec_left : 255u, r->rec[3], r->rec[4],
                                    r->fresh ? r->rec[5] : AR_KEEP };
            ar_copy(r, h, b, AR_BLOCK);
            r->blk_left = b[0];
            r->fresh = 0;
            h += AR_BLOCK;
            room -= AR_BLOCK;
        }
        if (!room) break;
        if (k > room) k = room;
        if (r->adpcm && k > r->blk_left) k = r->blk_left;
        ar_copy(r, h, p + done, k);
        r->head = h + k;
        done += k;
        if (r->adpcm) {
            r->blk_left = (uint8_t)(r->blk_left - k);
            r->rec_left = (uint16_t)(r->rec_left - k);
        }
    }
    return done;
}

/* The next sample (0-255), or -1 when none has arrived yet. */
static inline int ar_next(struct audio_ring *r)
{
    if (r->high) {
        r->high = 0;
        return nnv_adpcm_one(&r->dec, r->code >> 4);
    }
    uint32_t t = r->tail, m = r->size - 1u;
    if (r->adpcm && !r->left) {
        if (t == r->head) return -1;
        const uint8_t *b = r->buf;                   /* a block: its 4 bytes were queued together */
        r->left = b[t & m];
        uint8_t index = b[(t + 3u) & m];
        if (index != AR_KEEP) {
            r->dec.pred = (int16_t)(uint16_t)(b[(t + 1u) & m] | b[(t + 2u) & m] << 8);
            r->dec.index = index;
        }
        r->tail = t += AR_BLOCK;
    }
    if (t == r->head) return -1;
    uint8_t c = r->buf[t & m];
    r->tail = t + 1u;
    if (!r->adpcm) return c;
    --r->left;
    r->code = c;
    r->high = 1;
    return nnv_adpcm_one(&r->dec, c & 15u);
}

#endif
