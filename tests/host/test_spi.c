/* SPDX-License-Identifier: GPL-3.0-or-later */
/* drivers/spi.c against a simulated SPI1: a transfer that never finishes
 * (GO_BUSY stuck) fails its transaction, which deselects the device, resets
 * the controller, skips the transaction's later transfers (zeros, never the
 * last received word) and is reported by spi_end(); the next transaction
 * works again with the device's clock. */
#include <stdint.h>
#include <string.h>
#include "../../drivers/spi.h"
#include "../../arch/nuc140/regs.h"
#include "check.h"

static struct { uint32_t addr; volatile uint32_t value; } regs[16];
static int stuck, transfers, resets;

/* The hardware's side, run on each access to a register. */
volatile uint32_t *host_reg(uint32_t addr)
{
    int i = 0;
    while (i < 16 && regs[i].addr && regs[i].addr != addr) ++i;
    if (!regs[i].addr) regs[i].addr = addr;
    if (addr == SPI1_BASE && (regs[i].value & 1u) && !stuck) {     /* SPI_CNTRL, GO_BUSY: the transfer */
        regs[i].value &= ~1u;
        *host_reg(SPI1_BASE + 0x10u) = ~*host_reg(SPI1_BASE + 0x20u);   /* RX0 = ~TX0 */
        ++transfers;
    }
    if (addr == 0x5000000cu && (regs[i].value & BIT(13))) {         /* IPRSTC2, SPI1_RST */
        ++resets;
        *host_reg(SPI1_BASE) = 0;                                   /* the reset frees the controller */
        stuck = 0;
    }
    return &regs[i].value;
}

static uint32_t reg(uint32_t addr) { return *host_reg(addr); }
static int w5500_selected(void) { return !(reg(0x50004008u + 0x40u * PORTB) & BIT(10)); }

int main(void)
{
    spi_init();
    spi_set_divider(SPI_W5500, 1);

    spi_select(SPI_W5500);
    CHECK(w5500_selected());
    CHECK_EQ_INT(reg(SPI1_BASE + 0x04u), 1);                       /* its clock */
    CHECK_EQ_INT(spi_xfer8(0x12), (uint8_t)~0x12);
    uint8_t buf[6];
    spi_read(buf, sizeof buf);
    CHECK(buf[0] == 0xff && buf[4] == 0xff);
    CHECK_EQ_INT(spi_end(), 0);
    CHECK(!w5500_selected());
    CHECK_EQ_INT(spi_timeouts(), 0);

    /* Stuck in the middle of a transaction. */
    spi_select(SPI_W5500);
    CHECK_EQ_INT(spi_xfer8(0x34), (uint8_t)~0x34);
    stuck = 1;
    int before = transfers;
    CHECK_EQ_INT(spi_xfer8(0x56), 0);                              /* not the last word received */
    CHECK_EQ_INT(spi_timeouts(), 1);
    CHECK_EQ_INT(resets, 1);
    CHECK(!w5500_selected());                                       /* deselected at once */
    CHECK_EQ_INT(reg(SPI1_BASE + 0x04u), 1);                       /* the clock set again after the reset */
    memset(buf, 0xaa, sizeof buf);
    spi_read(buf, sizeof buf);                                      /* skipped: zeros */
    spi_write("abcdef", 6);
    CHECK_EQ_INT(transfers, before);
    for (unsigned i = 0; i < sizeof buf; ++i) CHECK_EQ_INT(buf[i], 0);
    CHECK_EQ_INT(spi_xfer8(0x78), 0);
    CHECK_EQ_INT(spi_end(), -1);
    CHECK_EQ_INT(spi_timeouts(), 1);

    /* The next transaction works. */
    spi_select(SPI_CH376);
    CHECK_EQ_INT(reg(SPI1_BASE + 0x04u), SPI_DEFAULT_DIVIDER);
    CHECK_EQ_INT(spi_xfer8(0x9a), (uint8_t)~0x9a);
    CHECK_EQ_INT(spi_end(), 0);
    spi_select(SPI_NONE);                                          /* the plain way to end one */
    return check_summary("test_spi");
}
