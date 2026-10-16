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

/* 0 (off), 2, 7 or 26 seconds: 2^14, 2^16 or 2^18 cycles of 10 kHz. */
int wdt_set(int s)
{
    unsigned wtis = s == 2 ? 5u : s == 7 ? 6u : s == 26 ? 7u : 0u;
    if (s && !wtis) return -1;
    regs_unlock();
    if (s) {
        CLK_PWRCON |= BIT(3);                       /* 10 kHz oscillator */
        while (!(CLK_CLKSTATUS & BIT(3))) { }
        CLK_CLKSEL1 = (CLK_CLKSEL1 & ~3u) | 3u;     /* watchdog clock: 10 kHz */
        CLK_APBCLK |= BIT(0);
        WDT_WTCR = WTCR_WTE | WTCR_WTRE | WTCR_WTR | (wtis << 8);
    } else {
        WDT_WTCR = 0;
    }
    regs_lock();
    seconds = (uint8_t)s;
    return 0;
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
