/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NNNIX_BOOT_UPDATE_H
#define NNNIX_BOOT_UPDATE_H
#include <stdint.h>

/* The bootloader's decisions, apart from the hardware (host-tested). */
struct boot_hw {
    void     (*spi_read)(uint32_t addr, void *buf, uint32_t n);
    int      (*spi_program)(uint32_t addr, const void *buf, uint32_t n);   /* bits 1 -> 0 only */
    uint32_t (*ap_read)(uint32_t addr);
    int      (*ap_erase)(uint32_t page);                                    /* 512-byte page */
    int      (*ap_write)(uint32_t addr, uint32_t word);
    void     (*log)(const char *text);
};

#define AP_SIZE (128u * 1024u)
#define AP_PAGE 512u

enum { BOOT_RUN_APROM, BOOT_STAY };
int boot_decide(const struct boot_hw *hw);
uint32_t boot_crc(uint32_t crc, const uint8_t *p, uint32_t n);   /* zlib CRC-32 step; start and end with ~ */

#endif
