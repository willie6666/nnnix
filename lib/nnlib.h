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
#define NN_LIB_VERSION 1u

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
};

/* NXE: a program linked at address 0. The loader copies the image to RAM,
 * adds the load address to the 32-bit words listed in the relocation table,
 * clears bss and starts _start(argc, argv, lib) as a child task.
 *   header | image (image bytes) | relocations (nrelocs x uint32 offsets) */
#define NXE_MAGIC   0x3145584eu      /* "NXE1" */
#define NXE_VERSION 1u
struct nxe_header {
    uint32_t magic;
    uint16_t version, lib_version;   /* lib_version: the nn_lib version it needs */
    uint32_t image, bss;             /* bytes; image is a multiple of 4 */
    uint32_t entry;                  /* offset of _start, Thumb bit set */
    uint32_t nrelocs;
    uint32_t stack;                  /* task stack bytes; 0 for the default */
    uint32_t crc;                    /* CRC-32 (zlib) of the image and the relocations */
};

#endif
