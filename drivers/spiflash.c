/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "spiflash.h"
#include "../arch/nuc140/regs.h"
#include "../arch/nuc140/arch.h"
#include "../kernel/task.h"

#define SPI2_BASE 0x40130000u
#define DIVIDER   1u                 /* 50.19 MHz / 4 = 12.5 MHz (the W25Q16 reads at up to 50) */
enum { CMD_WRITE_ENABLE = 0x06, CMD_READ_STATUS = 0x05, CMD_READ = 0x03, CMD_PAGE_PROGRAM = 0x02,
       CMD_SECTOR_ERASE = 0x20, CMD_JEDEC_ID = 0x9f, CMD_RELEASE_POWER_DOWN = 0xab };

static void cs(int active)
{
    if (active) GPIO_DOUT(PORTD) &= ~BIT(0);
    else GPIO_DOUT(PORTD) |= BIT(0);
}

/* A transfer the controller never finished (it should take microseconds):
 * the operation it belongs to reports an error instead of returning whatever
 * the receive register held. Cleared at the start of each operation. */
static uint8_t timed_out;
static uint32_t timeouts;
uint32_t spiflash_timeouts(void) { return timeouts; }

static int finished(void)
{
    for (uint32_t n = 0; n < 10000u; ++n) if (!(SPI_CNTRL(SPI2_BASE) & 1u)) return 1;
    timed_out = 1;
    ++timeouts;
    return 0;
}

static uint8_t xfer(uint8_t v)
{
    SPI_TX0(SPI2_BASE) = v;
    SPI_CNTRL(SPI2_BASE) = BIT(2) | (8u << 3) | 1u;
    finished();
    return (uint8_t)SPI_RX0(SPI2_BASE);
}

static void command_addr(uint8_t cmd, uint32_t addr)
{
    xfer(cmd);
    xfer((uint8_t)(addr >> 16));
    xfer((uint8_t)(addr >> 8));
    xfer((uint8_t)addr);
}

static void simple(uint8_t cmd) { cs(1); xfer(cmd); cs(0); }

static int wait_ready(uint32_t timeout_ms)
{
    uint32_t start = jiffies;
    for (;;) {
        cs(1);
        xfer(CMD_READ_STATUS);
        uint8_t status = xfer(0);
        cs(0);
        if (!(status & 1u)) return 0;           /* BUSY clear */
        if (jiffies - start > timeout_ms) return -1;
        if (timeout_ms > 10u) msleep(1);        /* erases take tens of ms: let others run */
    }
}

void spiflash_init(void)
{
    regs_unlock();
    CLK_APBCLK |= BIT(14);                      /* SPI2 clock */
    CLK_CLKSEL1 |= BIT(6);                      /* from HCLK */
    regs_lock();
    GPIO_DOUT(PORTD) |= BIT(0);
    gpio_mode(PORTD, 0, GPIO_OUTPUT);           /* chip select as a plain GPIO */
    SYS_GPD_MFP |= BIT(1) | BIT(2) | BIT(3);    /* SPI2 CLK, MISO, MOSI */
    SPI_CNTRL(SPI2_BASE) = BIT(2) | (8u << 3);  /* master, mode 0, 8 bits */
    SPI_DIVIDER(SPI2_BASE) = DIVIDER;
    SPI_SSR(SPI2_BASE) = 0;
    cs(0);
    simple(CMD_RELEASE_POWER_DOWN);
    delay_us(5);
}

uint32_t spiflash_jedec_id(void)
{
    cs(1);
    xfer(CMD_JEDEC_ID);
    uint32_t id = (uint32_t)xfer(0) << 16;
    id |= (uint32_t)xfer(0) << 8;
    id |= xfer(0);
    cs(0);
    return id;
}

uint32_t spiflash_size(void)
{
    uint32_t id = spiflash_jedec_id(), cap = id & 0xffu;
    if (id == 0 || id == 0xffffffu || cap < 0x10u || cap > 0x18u) return 0;
    return 1u << cap;                           /* Winbond: 0x15 = 2 MiB, 0x17 = 8 MiB */
}

/* Reads move 32 bits per SPI transfer (the length field 0 means 32; the
 * first byte on the wire is the top one): littlefs scans whole metadata
 * blocks, and per-byte transfers spent more time starting than shifting. */
int spiflash_read(uint32_t addr, void *buf, uint32_t n)
{
    uint8_t *p = buf;
    timed_out = 0;
    cs(1);
    command_addr(CMD_READ, addr);
    for (; n >= 4u; n -= 4u, p += 4) {
        SPI_TX0(SPI2_BASE) = 0;
        SPI_CNTRL(SPI2_BASE) = BIT(2) | 1u;
        finished();
        uint32_t w = SPI_RX0(SPI2_BASE);
        p[0] = (uint8_t)(w >> 24); p[1] = (uint8_t)(w >> 16); p[2] = (uint8_t)(w >> 8); p[3] = (uint8_t)w;
    }
    while (n--) *p++ = xfer(0);
    cs(0);
    return timed_out ? -1 : 0;
}

int spiflash_program(uint32_t addr, const void *buf, uint32_t n)
{
    const uint8_t *p = buf;
    timed_out = 0;
    while (n) {
        uint32_t room = SPIFLASH_PAGE - (addr % SPIFLASH_PAGE), chunk = n < room ? n : room;
        simple(CMD_WRITE_ENABLE);
        cs(1);
        command_addr(CMD_PAGE_PROGRAM, addr);
        for (uint32_t i = 0; i < chunk; ++i) xfer(p[i]);
        cs(0);
        if (wait_ready(10) || timed_out) return -1;   /* page program: 0.7 ms typical, 3 ms max */
        addr += chunk; p += chunk; n -= chunk;
    }
    return 0;
}

int spiflash_erase_sector(uint32_t addr)
{
    timed_out = 0;
    simple(CMD_WRITE_ENABLE);
    cs(1);
    command_addr(CMD_SECTOR_ERASE, addr & ~(SPIFLASH_SECTOR - 1u));
    cs(0);
    return wait_ready(500) || timed_out ? -1 : 0;   /* 45 ms typical, 400 ms max */
}
