/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Flash memory controller: reading LDROM and CONFIG through ISP, and the
 * resets that pick where the chip starts (see boot/boot.c). */
#include "arch.h"
#include "regs.h"

#define CLK_AHBCLK  REG(0x50000204u)
#define SYS_IPRSTC1 REG(0x50000008u)
#define FMC_ISPCON  REG(0x5000c000u)
#define FMC_ISPADR  REG(0x5000c004u)
#define FMC_ISPDAT  REG(0x5000c008u)
#define FMC_ISPCMD  REG(0x5000c00cu)
#define FMC_ISPTRG  REG(0x5000c010u)

uint32_t fmc_read(uint32_t addr)
{
    uint32_t s = irq_save();
    regs_unlock();
    CLK_AHBCLK |= BIT(2);                           /* flash controller clock */
    FMC_ISPCON |= BIT(0);                           /* ISPEN */
    FMC_ISPCMD = 0x00;                              /* read */
    FMC_ISPADR = addr;
    FMC_ISPTRG = 1;
    __asm volatile("isb");
    while (FMC_ISPTRG) { }
    uint32_t v = FMC_ISPDAT;
    FMC_ISPCON &= ~BIT(0);
    regs_lock();
    irq_restore(s);
    return v;
}

/* BS = 1: the next reset (except power-on) starts the LDROM bootloader. */
void arch_reboot_bootloader(void)
{
    irq_disable();
    regs_unlock();
    CLK_AHBCLK |= BIT(2);
    FMC_ISPCON |= BIT(0) | BIT(1);
    arch_reboot();
}

/* CHIP_RST: like a power-on reset; CONFIG0 is read again and decides. */
void arch_reboot_chip(void)
{
    irq_disable();
    regs_unlock();
    SYS_IPRSTC1 = BIT(0);
    for (;;) { }
}
