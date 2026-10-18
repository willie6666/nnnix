/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NNNIX_NNLIB_H
#define NNNIX_NNLIB_H
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include "time.h"

/* Loaded programs (M6) do not carry a C library: the firmware's own functions,
 * which run from flash, are handed to them as this table (the third argument
 * of _start). Entries are only ever added at the end; `size` tells a program
 * how many there are, and NXE files name the version they need. */
#define NN_LIB_MAGIC   0x42494c4eu   /* "NLIB" */
#define NN_LIB_VERSION 2u           /* 2: the fonts */

struct nn_lib {
    uint32_t magic, version, size;   /* size: bytes of this table */
    int   (*vprintf)(const char *fmt, va_list ap);                 /* to fd 1 */
    int   (*vsnprintf)(char *buf, size_t n, const char *fmt, va_list ap);
    void *(*memcpy)(void *dst, const void *src, size_t n);
    void *(*memmove)(void *dst, const void *src, size_t n);
    void *(*memset)(void *dst, int c, size_t n);
    int   (*memcmp)(const void *a, const void *b, size_t n);
    size_t (*strlen)(const char *s);
    int   (*strcmp)(const char *a, const char *b);
    int   (*strncmp)(const char *a, const char *b, size_t n);
    char *(*strcpy)(char *dst, const char *src);
    char *(*strchr)(const char *s, int c);
    uint32_t (*uidiv)(uint32_t n, uint32_t d);                     /* the Cortex-M0 cannot divide */
    uint64_t (*uidivmod)(uint32_t n, uint32_t d);                  /* quotient low, remainder high */
    int32_t  (*idiv)(int32_t n, int32_t d);
    uint64_t (*idivmod)(int32_t n, int32_t d);
    const char *(*errstr)(int error);                              /* "no such file or directory" */
    void     (*gmtime)(uint32_t t, struct nn_tm *out);
    uint32_t (*mktime)(const struct nn_tm *t);
    void     (*format_time)(uint32_t t, char *buf);                /* 20 bytes */
    /* Version 2: the console's fonts, characters 0x20-0x7f (index c - 0x20).
     * 3x5: 15 bits a glyph, column 0 in bits 0-4 (bit 0 the top), column 1 in
     * bits 5-9, column 2 in bits 10-14. 5x7: a byte per column, bit 0 the top. */
    const uint16_t *font3x5;
    const uint8_t (*font5x7)[5];
};

/* NXE: a program built with -fropi -frwpi (sdk/Makefile): its code and
 * read-only data (text) reach each other PC-relative, and its data and bss
 * through r9, the static base. So text can run from anywhere and is shared by
 * every run: the loader keeps it in APROM above the firmware (kernel/xip.c),
 * written once and found again by its CRC, or in RAM when APROM has no room.
 * Each run gets only data and bss in RAM; the loader starts
 * _start(argc, argv, lib) with r9 pointing at them.
 *   header | text | text relocations | data | data relocations
 * Text relocations: offsets of words in text that hold a text address (tables
 * of pointers to strings or functions); the loader adds where text is. Data
 * relocations: offsets of words in data, bit 0 set when the word holds an
 * offset into data or bss (the loader adds where data is), clear when it holds
 * a text address. data_crc covers the rest of the header too (the loader
 * reads data on every run, text only when no APROM slot has it), so the entry,
 * sizes and relocation counts are checked each time. */
#define NXE_MAGIC   0x3145584eu      /* "NXE1" */
#define NXE_VERSION 3u
#define NXE_RELOC_DATA 1u            /* in a data relocation: the word points into data or bss */
struct nxe_header {
    uint32_t magic;
    uint16_t version, lib_version;   /* lib_version: the nn_lib version it needs */
    uint32_t text, data, bss;        /* bytes, multiples of 4 */
    uint32_t entry;                  /* offset of _start in text, Thumb bit set */
    uint16_t text_relocs, data_relocs;
    uint32_t stack;                  /* task stack bytes; 0 for the default */
    uint32_t text_crc;               /* CRC-32 (zlib) of text and its relocations: names the APROM copy */
    uint32_t data_crc;               /* CRC-32 of the header before it, data and its relocations */
};

#endif
