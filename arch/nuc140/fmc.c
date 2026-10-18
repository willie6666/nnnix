/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Flash memory controller: reading LDROM and CONFIG through ISP, erasing and
 * writing APROM pages above the firmware (kernel/xip.c), and the resets that
 * pick where the chip starts (see boot/boot.c). */
#include "arch.h"
#include "regs.h"

#define CLK_AHBCLK  REG(0x50000204u)
#define SYS_IPRSTC1 REG(0x50000008u)
#define FMC_ISPCON  REG(0x5000c000u)
#define FMC_ISPADR  REG(0x5000c004u)
#define FMC_ISPDAT  REG(0x5000c008u)
#define FMC_ISPCMD  REG(0x5000c00cu)
#define FMC_ISPTRG  REG(0x5000c010u)
#define ISPCON_ISPEN BIT(0)
#define ISPCON_APUEN BIT(3)                         /* APROM may be written while running from it */
#define ISPCON_ISPFF BIT(6)
#define SYST_COUNTFLAG BIT(16)

extern volatile uint32_t jiffies;

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

/* Start an ISP command and wait for it. This runs from a copy on the stack
 * (link.ld places it between __isp_run and __isp_run_end): nothing may be
 * fetched from APROM until an APROM command ends (without APUEN the chip
 * refuses it outright, ISPFF). Called with interrupts off; counts the SysTick
 * periods the wait took, so the lost ticks can be added to jiffies (an erase
 * takes 20 ms). Position-independent: its constants are PC-relative. */
__attribute__((section(".isp_run"), noinline, used))
static uint32_t isp_run(void)
{
    uint32_t wraps = 0;
    FMC_ISPTRG = 1;
    __asm volatile("isb");
    while (FMC_ISPTRG)
        if (SYST_CSR & SYST_COUNTFLAG) ++wraps;
    return wraps;
}

extern const uint32_t __isp_run[], __isp_run_end[];
#define ISP_RUN_WORDS 12u                           /* link.ld checks that it fits */

/* An APROM erase (0x22) or word write (0x21): 0, or -1 if the chip refused. */
static int isp_aprom(uint32_t cmd, uint32_t addr, uint32_t data)
{
    uint32_t code[ISP_RUN_WORDS];
    for (uint32_t i = 0; i < (uint32_t)(__isp_run_end - __isp_run); ++i) code[i] = __isp_run[i];
    uint32_t (*run)(void) = (uint32_t (*)(void))((uintptr_t)code | 1u);
    uint32_t s = irq_save();
    (void)(SYST_CSR & SYST_COUNTFLAG);              /* clear: count only this command's periods */
    regs_unlock();
    CLK_AHBCLK |= BIT(2);
    FMC_ISPCON |= ISPCON_ISPEN | ISPCON_APUEN;
    FMC_ISPCMD = cmd;
    FMC_ISPADR = addr;
    FMC_ISPDAT = data;
    __asm volatile("dsb\n isb" ::: "memory");      /* the copy is complete before it runs */
    uint32_t wraps = run();
    int failed = (FMC_ISPCON & ISPCON_ISPFF) != 0;
    FMC_ISPCON = (FMC_ISPCON & ~(ISPCON_ISPEN | ISPCON_APUEN)) | ISPCON_ISPFF;   /* ISPFF: write 1 to clear */
    regs_lock();
    if (wraps > 1) jiffies += wraps - 1;            /* the pending SysTick interrupt adds one */
    irq_restore(s);
    return failed ? -1 : 0;
}

int fmc_erase(uint32_t page) { return isp_aprom(0x22, page, 0); }
int fmc_write(uint32_t addr, uint32_t word) { return isp_aprom(0x21, addr, word); }

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
