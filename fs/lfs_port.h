/* SPDX-License-Identifier: GPL-3.0-or-later */
/* littlefs utility definitions for nnnix (selected with -DLFS_CONFIG=lfs_port.h).
 * Same interface as littlefs's lfs_util.h, without libc and without bit-scan
 * intrinsics, which the Cortex-M0 lacks. */
#ifndef NNNIX_LFS_PORT_H
#define NNNIX_LFS_PORT_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "../lib/string.h"

#define LFS_NO_MALLOC
#define LFS_TRACE(...)
#define LFS_DEBUG(...)
#define LFS_WARN(...)
#define LFS_ERROR(...)
#define LFS_ASSERT(test) ((void)0)

static inline uint32_t lfs_max(uint32_t a, uint32_t b) { return a > b ? a : b; }
static inline uint32_t lfs_min(uint32_t a, uint32_t b) { return a < b ? a : b; }
static inline uint32_t lfs_aligndown(uint32_t a, uint32_t alignment) { return a - (a % alignment); }
static inline uint32_t lfs_alignup(uint32_t a, uint32_t alignment) { return lfs_aligndown(a + alignment - 1, alignment); }

/* Smallest power of two >= a, as an exponent. */
static inline uint32_t lfs_npw2(uint32_t a)
{
    uint32_t r = 0, s;
    a -= 1;
    s = (a > 0xffff) << 4; a >>= s; r |= s;
    s = (a > 0xff) << 3; a >>= s; r |= s;
    s = (a > 0xf) << 2; a >>= s; r |= s;
    s = (a > 0x3) << 1; a >>= s; r |= s;
    return (r | (a >> 1)) + 1;
}

static inline uint32_t lfs_ctz(uint32_t a) { return lfs_npw2((a & -a) + 1) - 1; }

static inline uint32_t lfs_popc(uint32_t a)
{
    a = a - ((a >> 1) & 0x55555555);
    a = (a & 0x33333333) + ((a >> 2) & 0x33333333);
    return (((a + (a >> 4)) & 0xf0f0f0f) * 0x1010101) >> 24;
}

static inline int lfs_scmp(uint32_t a, uint32_t b) { return (int)(unsigned)(a - b); }

/* The Cortex-M0 is little endian. */
static inline uint32_t lfs_fromle32(uint32_t a) { return a; }
static inline uint32_t lfs_tole32(uint32_t a) { return a; }
static inline uint32_t lfs_frombe32(uint32_t a)
{
    return (a >> 24) | ((a >> 8) & 0xff00u) | ((a << 8) & 0xff0000u) | (a << 24);
}
static inline uint32_t lfs_tobe32(uint32_t a) { return lfs_frombe32(a); }

uint32_t lfs_crc(uint32_t crc, const void *buffer, size_t size);

/* All buffers are supplied statically or per open file by nnnix. */
static inline void *lfs_malloc(size_t size) { (void)size; return NULL; }
static inline void lfs_free(void *p) { (void)p; }

#endif
