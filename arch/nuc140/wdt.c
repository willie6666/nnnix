/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Hardware watchdog on the 10 kHz internal oscillator. The kernel daemon
 * restarts its count every second; if the daemon stops running (a task that
 * never yields at a higher priority, a hang with interrupts off, a lockup),
 * the chip resets. The count pauses while a debugger halts the CPU. */
#include "arch.h"
#include "regs.h"

#define WTCR_WTR  BIT(0)
#define WTCR_WTRE BIT(1)
#define WTCR_WTRF BIT(2)
#define WTCR_WTIF BIT(3)
#define WTCR_WTWKF BIT(5)
#define WTCR_WTE  BIT(7)

static volatile uint8_t seconds;     /* 0: off */

int wdt_seconds(void) { return seconds; }

/* 0 (off), 2, 7 or 26 seconds: 2^14, 2^16 or 2^18 cycles of 10 kHz. The
 * write-protected registers are changed with interrupts off: the kernel
 * daemon's kick locks the protection again, which would otherwise silently
 * drop a write here. Returns -1 if the hardware did not take the setting. */
int wdt_set(int s)
{
    unsigned wtis = s == 2 ? 5u : s == 7 ? 6u : s == 26 ? 7u : 0u;
    if (s && !wtis) return -1;
    if (s) {                                        /* 10 kHz oscillator first, outside the critical section */
        uint32_t f = irq_save();
        regs_unlock();
        CLK_PWRCON |= BIT(3);
        regs_lock();
        irq_restore(f);
        while (!(CLK_CLKSTATUS & BIT(3))) { }
    }
    uint32_t f = irq_save();
    regs_unlock();
    if (s) {
        CLK_CLKSEL1 = (CLK_CLKSEL1 & ~3u) | 3u;     /* watchdog clock: 10 kHz */
        CLK_APBCLK |= BIT(0);
        WDT_WTCR = WTCR_WTE | WTCR_WTRE | WTCR_WTR | (wtis << 8);
    } else {
        WDT_WTCR = 0;
    }
    regs_lock();
    int ok = s ? (WDT_WTCR & WTCR_WTE) && ((WDT_WTCR >> 8) & 7u) == wtis : !(WDT_WTCR & WTCR_WTE);
    if (ok) seconds = (uint8_t)s;
    irq_restore(f);
    return ok ? 0 : -1;
}

void wdt_kick(void)
{
    if (!seconds) return;
    uint32_t s = irq_save();
    regs_unlock();
    WDT_WTCR = (WDT_WTCR & ~(WTCR_WTRF | WTCR_WTIF | WTCR_WTWKF)) | WTCR_WTR;
    regs_lock();
    irq_restore(s);
}
