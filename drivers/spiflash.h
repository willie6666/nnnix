/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NNNIX_SPIFLASH_H
#define NNNIX_SPIFLASH_H
#include <stdint.h>

/* Onboard Winbond SPI NOR flash on SPI2 (PD0 CS as GPIO, PD1 CLK, PD2 MISO,
 * PD3 MOSI). 256-byte pages, 4 KiB erase sectors. Only one task may use it at
 * a time (the file system holds its own lock). */
#define SPIFLASH_PAGE   256u
#define SPIFLASH_SECTOR 4096u

void     spiflash_init(void);
uint32_t spiflash_jedec_id(void);     /* manufacturer << 16 | type << 8 | capacity */
uint32_t spiflash_size(void);         /* bytes, from the JEDEC capacity code; 0 if absent */
int      spiflash_read(uint32_t addr, void *buf, uint32_t n);          /* 0, or -1 if the SPI controller hung */
int      spiflash_program(uint32_t addr, const void *buf, uint32_t n);   /* within pages; 0 or -1 */
int      spiflash_erase_sector(uint32_t addr);                          /* 0 or -1 */
uint32_t spiflash_timeouts(void);     /* SPI transfers that never finished, since boot */

#endif
