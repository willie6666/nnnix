/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NNNIX_LCD_H
#define NNNIX_LCD_H
#include <stdint.h>

/* Onboard 128x64 monochrome LCD on SPI3 (PD8 CS, PD9 CLK, PD11 MOSI), 9-bit
 * frames: bit 8 selects data (1) or command (0). 8 pages of 8 pixel rows; the
 * board maps logical x to controller column 129 - x. The console draws its
 * text itself (kernel/console.c). */
void lcd_init(void);
void lcd_clear(void);
/* Pixel columns of one page from x_hi down to lower x, one byte each (bit 0
 * the top row): begin, a byte per column, end. */
void lcd_run_begin(int page, int x_hi);
void lcd_run_byte(uint8_t b);
void lcd_run_end(void);
/* Graphics: n bytes of a 1024-byte frame starting at byte off (< 1024). Byte i
 * holds x = i % 128 of page i / 128, bit 0 the top pixel row; set bits are dark. */
void lcd_bytes(uint32_t off, const uint8_t *p, uint32_t n);
uint32_t lcd_timeouts(void);         /* SPI transfers that never finished, since boot */
#define LCD_FRAME 1024u

#endif
