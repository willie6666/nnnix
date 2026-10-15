/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NNNIX_SPI_H
#define NNNIX_SPI_H
#include <stddef.h>
#include <stdint.h>

/* SPI1, mode 0, MSB first. Clock = HCLK / ((divider + 1) * 2):
 * 0 -> 25.1 MHz, 1 -> 12.5 MHz, 2 -> 8.4 MHz, 4 -> 5.0 MHz.
 * CH376S works up to 12.5 MHz and only with byte transfers; W5500 works at 25 MHz. */
enum { SPI_NONE = -1, SPI_CH376 = 0, SPI_W5500 = 1 };
#define SPI_DEFAULT_DIVIDER 4u

void spi_init(void);
/* Each device keeps its own clock; it is applied when the device is selected. */
void spi_set_divider(int device, unsigned divider);
unsigned spi_divider(int device);
/* Selecting a device takes the bus lock; spi_select(SPI_NONE) releases it.
 * Keep each select/deselect pair inside one task. */
void spi_select(int device);
uint8_t spi_xfer8(uint8_t byte);
void spi_write(const void *buf, size_t n);   /* 32-bit bursts: W5500 only */
void spi_read(void *buf, size_t n);          /* 32-bit bursts: W5500 only */
uint32_t spi_timeouts(void);

#endif
