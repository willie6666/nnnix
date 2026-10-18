/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NNNIX_SPI_H
#define NNNIX_SPI_H
#include <stddef.h>
#include <stdint.h>

/* SPI1, mode 0, MSB first. Clock = HCLK / ((divider + 1) * 2):
 * 0 -> 25.1 MHz, 1 -> 12.5 MHz, 2 -> 8.4 MHz, 4 -> 5.0 MHz.
 * CH376S works up to 12.5 MHz and only with byte transfers. W5500 reads at
 * 25 MHz showed about 6 bad bytes per 10 MiB on the breadboard wiring and none
 * at 12.5 MHz, so 12.5 MHz is its default. */
enum { SPI_NONE = -1, SPI_CH376 = 0, SPI_W5500 = 1 };
#define SPI_DEFAULT_DIVIDER 4u        /* CH376S */
#define SPI_W5500_DIVIDER   1u

void spi_init(void);
/* Each device keeps its own clock; it is applied when the device is selected. */
void spi_set_divider(int device, unsigned divider);
unsigned spi_divider(int device);
/* Selecting a device takes the bus lock and starts a transaction;
 * spi_select(SPI_NONE) or spi_end() ends it and releases the lock. Keep each
 * pair inside one task. A transfer that times out fails the transaction: the
 * device is deselected, the controller reset, and the transaction's later
 * transfers skipped (reads give zeros). */
void spi_select(int device);
int spi_end(void);                           /* spi_select(SPI_NONE): 0, or -1 when the transaction failed */
uint8_t spi_xfer8(uint8_t byte);
void spi_write(const void *buf, size_t n);   /* 32-bit bursts: W5500 only */
void spi_read(void *buf, size_t n);          /* 32-bit bursts: W5500 only */
uint32_t spi_timeouts(void);

#endif
