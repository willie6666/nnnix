/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NNNIX_FWUPDATE_H
#define NNNIX_FWUPDATE_H
#include <stdint.h>

/* Firmware updates (M7). nnnix copies a new APROM image into a region at the
 * end of the onboard SPI flash (outside littlefs) and writes a record; the
 * LDROM bootloader programs APROM from it and marks the record done. The
 * record is written last, so an interrupted copy is ignored; the bootloader
 * only marks it done after APROM verifies, so an interrupted programming
 * run is simply repeated at the next boot.
 *
 *   0x1df000  record sector (4 KiB)
 *   0x1e0000  image, up to 128 KiB (one APROM)
 *
 * State words only lose bits (NOR flash), so no erase is needed to move on:
 * erased 0xffffffff -> staged -> done. */
#define FW_RECORD_ADDR 0x1df000u
#define FW_IMAGE_ADDR  0x1e0000u
#define FW_IMAGE_MAX   (128u * 1024u)
#define FW_FS_END      FW_RECORD_ADDR    /* littlefs uses the flash below this */

#define FW_MAGIC       0x4450554eu       /* "NUPD" */
#define FW_STAGED      0xffff0000u       /* image ready: program APROM */
#define FW_DONE        0x00000000u       /* APROM holds this image */

struct fw_record {
    uint32_t magic;
    uint32_t size;                       /* bytes, a multiple of 4 */
    uint32_t crc;                        /* CRC-32 (zlib) of the image */
    uint32_t check;                      /* ~(magic ^ size ^ crc): the record was written whole */
    uint32_t state;
};

static inline int fw_record_valid(const struct fw_record *r)
{
    return r->magic == FW_MAGIC && r->check == ~(r->magic ^ r->size ^ r->crc) && r->size >= 8u &&
           r->size <= FW_IMAGE_MAX && !(r->size % 4u);
}

/* A plausible APROM image: initial stack pointer in SRAM, reset handler a
 * Thumb address inside the image. */
static inline int fw_vectors_valid(uint32_t sp, uint32_t reset, uint32_t size)
{
    return sp > 0x20000000u && sp <= 0x20004000u && (reset & 1u) && reset < size;
}

#endif
