#include "spi.h"
#include "../arch/nuc140/regs.h"

#define MODE0 BIT(2)                 /* TX_NEG: drive on falling edge, sample on rising */
static uint32_t timeouts;

void spi_init(void)
{
    SPI_CNTRL(SPI1_BASE) = MODE0 | (8u << 3);
    SPI_DIVIDER(SPI1_BASE) = SPI_DEFAULT_DIVIDER;
    SPI_SSR(SPI1_BASE) = 0;          /* chip selects are plain GPIOs */
}

void spi_set_divider(unsigned divider) { SPI_DIVIDER(SPI1_BASE) = divider & 0xffu; }
unsigned spi_divider(void) { return SPI_DIVIDER(SPI1_BASE) & 0xffu; }
uint32_t spi_timeouts(void) { return timeouts; }

void spi_select(int device)
{
    GPIO_DOUT(PORTC) |= BIT(8);
    GPIO_DOUT(PORTB) |= BIT(10);
    if (device == SPI_CH376) GPIO_DOUT(PORTC) &= ~BIT(8);
    if (device == SPI_W5500) GPIO_DOUT(PORTB) &= ~BIT(10);
}

static uint32_t go(uint32_t cntrl)
{
    SPI_CNTRL(SPI1_BASE) = cntrl;
    SPI_CNTRL(SPI1_BASE) = cntrl | 1u;
    for (uint32_t n = 0; SPI_CNTRL(SPI1_BASE) & 1u; )
        if (++n > 100000u) { timeouts++; break; }
    return SPI_RX0(SPI1_BASE);
}

uint8_t spi_xfer8(uint8_t byte)
{
    SPI_TX0(SPI1_BASE) = byte;
    return (uint8_t)go(MODE0 | (8u << 3));
}

void spi_write(const void *buf, size_t n)
{
    const uint8_t *p = buf;
    for (; n >= 4; p += 4, n -= 4) {
        SPI_TX0(SPI1_BASE) = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
        go(MODE0);                   /* TX_BIT_LEN 0 = 32 bits */
    }
    while (n--) spi_xfer8(*p++);
}

void spi_read(void *buf, size_t n)
{
    uint8_t *p = buf;
    for (; n >= 4; p += 4, n -= 4) {
        SPI_TX0(SPI1_BASE) = 0;
        uint32_t w = go(MODE0);
        p[0] = (uint8_t)(w >> 24); p[1] = (uint8_t)(w >> 16); p[2] = (uint8_t)(w >> 8); p[3] = (uint8_t)w;
    }
    while (n--) *p++ = spi_xfer8(0);
}
