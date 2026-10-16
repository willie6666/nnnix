/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Initialisation sequence from the board vendor's sample (bias, contrast,
 * mapping, display on). */
#include "lcd.h"
#include "font5x7.h"
#include "../arch/nuc140/regs.h"

#define SPI3_BASE 0x40134000u
#define DIVIDER   7u                 /* 50.19 MHz / 16 = 3.1 MHz */

static void cs(int active)
{
    if (active) GPIO_DOUT(PORTD) &= ~BIT(8);
    else GPIO_DOUT(PORTD) |= BIT(8);
}

static void send9(uint32_t v)
{
    SPI_TX0(SPI3_BASE) = v;
    SPI_CNTRL(SPI3_BASE) = BIT(2) | (9u << 3) | 1u;
    for (uint32_t n = 0; SPI_CNTRL(SPI3_BASE) & 1u && n < 10000u; ++n) { }
}

static void command(uint8_t c) { send9(c); }
static void data(uint8_t d) { send9(0x100u | d); }

static void address(uint8_t page, uint8_t column)
{
    command((uint8_t)(0xb0u | page));
    command((uint8_t)(0x10u | (column >> 4)));
    command((uint8_t)(column & 0x0fu));
}

void lcd_init(void)
{
    regs_unlock();
    CLK_APBCLK |= BIT(15);                          /* SPI3 clock */
    CLK_CLKSEL1 |= BIT(7);                          /* from HCLK */
    regs_lock();
    GPIO_DOUT(PORTD) |= BIT(8);
    gpio_mode(PORTD, 8, GPIO_OUTPUT);               /* chip select as a plain GPIO */
    SYS_GPD_MFP |= BIT(9) | BIT(11);                /* SPI3 CLK, MOSI */
    SPI_CNTRL(SPI3_BASE) = BIT(2) | (9u << 3);      /* master, mode 0, 9-bit frames */
    SPI_DIVIDER(SPI3_BASE) = DIVIDER;
    SPI_SSR(SPI3_BASE) = 0;
    cs(1);
    command(0xeb);                                  /* bias */
    command(0x81);                                  /* contrast, followed by its value */
    command(0xa0);
    command(0xc0);                                  /* mapping */
    command(0xaf);                                  /* display on */
    cs(0);
    lcd_clear();
}

void lcd_clear(void)
{
    cs(1);
    for (uint8_t page = 0; page < 8; ++page) {
        address(page, 0);
        for (int i = 0; i < 132; ++i) data(0);
    }
    cs(0);
}

void lcd_char(int col, int row, char c, int cursor)
{
    if (col < 0 || col >= LCD_COLS || row < 0 || row >= LCD_ROWS) return;
    uint8_t ch = (uint8_t)c;
    const uint8_t *glyph = font5x7[(ch >= 0x20 && ch <= 0x7f ? ch : '?') - 0x20];
    uint8_t under = cursor ? 0x80u : 0;
    int x = col * 6;
    /* Logical columns x..x+5 are controller columns 129-x down to 124-x; write
     * them upwards from 124-x: the spacing column first, then the glyph reversed. */
    cs(1);
    address((uint8_t)row, (uint8_t)(124 - x));
    data(under);
    for (int i = 4; i >= 0; --i) data((uint8_t)(glyph[i] | under));
    cs(0);
}

void lcd_bytes(uint32_t off, const uint8_t *p, uint32_t n)
{
    cs(1);
    while (n && off < LCD_FRAME) {
        uint32_t x = off % 128u, run = 128u - x;
        if (run > n) run = n;
        /* x..x+run-1 are controller columns 129-x down to 130-x-run: write
         * upwards from the lowest, so the bytes go out in reverse. */
        address((uint8_t)(off / 128u), (uint8_t)(130u - x - run));
        for (uint32_t i = run; i--;) data(p[i]);
        off += run;
        p += run;
        n -= run;
    }
    cs(0);
}
