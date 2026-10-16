/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "rtc.h"
#include "../arch/nuc140/arch.h"
#include "../arch/nuc140/regs.h"
#include "../kernel/klog.h"
#include "../lib/time.h"

#define INIT_KEY   0xa5eb1357u
#define ACCESS_KEY 0xa965u
#define LXT_WAIT_MS 5000u

enum { STARTING, RUNNING, NO_CRYSTAL };
static uint8_t state;
static uint32_t soft_base, soft_at;  /* time_now() = soft_base + (jiffies - soft_at) / 1000 */

void rtc_init(void)
{
    regs_unlock();
    CLK_PWRCON |= BIT(1);                          /* 32.768 kHz crystal on */
    CLK_APBCLK |= BIT(1);                          /* RTC register clock */
    regs_lock();
}

int rtc_running(void) { return state == RUNNING; }

static unsigned bcd(uint32_t v) { return (v >> 4) * 10u + (v & 15u); }
static uint32_t to_bcd(unsigned v) { return (uint32_t)((v / 10u) << 4 | v % 10u); }

static uint32_t rtc_read(void)
{
    uint32_t c, t;
    do { c = RTC_CLR; t = RTC_TLR; } while (c != RTC_CLR);   /* midnight between the reads */
    struct nn_tm tm = {
        .year = (uint16_t)(2000u + bcd(c >> 16 & 0xffu)), .mon = (uint8_t)bcd(c >> 8 & 0x1fu),
        .mday = (uint8_t)bcd(c & 0x3fu), .hour = (uint8_t)bcd(t >> 16 & 0x3fu),
        .min = (uint8_t)bcd(t >> 8 & 0x7fu), .sec = (uint8_t)bcd(t & 0x7fu),
    };
    return nn_mktime(&tm);
}

static int access(void)
{
    RTC_AER = ACCESS_KEY;
    for (uint32_t start = jiffies; !(RTC_AER & BIT(16));)
        if (jiffies - start > 50u) return -1;
    return 0;
}

static void soft_set(uint32_t t) { soft_base = t; soft_at = jiffies; }

void rtc_poll(void)
{
    if (state != STARTING) {
        /* Fold elapsed seconds in so jiffies wrapping (49 days) does not matter. */
        uint32_t secs = (jiffies - soft_at) / 1000u;
        soft_base += secs;
        soft_at += secs * 1000u;
        return;
    }
    if (CLK_CLKSTATUS & BIT(1)) {
        if (!(RTC_INIR & 1u)) {                    /* first start since power-on */
            RTC_INIR = INIT_KEY;
            for (uint32_t start = jiffies; !(RTC_INIR & 1u) && jiffies - start < 100u;) { }
        }
        if (!(RTC_INIR & 1u)) return;
        if (!(RTC_TSSR & 1u) && !access()) RTC_TSSR = 1u;    /* 24-hour mode */
        state = RUNNING;
        soft_set(rtc_read());
        char buf[20];
        nn_format_time(soft_base, buf);
        kprintf("rtc: 32.768 kHz crystal running, %s UTC\n", buf);
    } else if (jiffies > LXT_WAIT_MS) {
        state = NO_CRYSTAL;
        kprintf("rtc: no 32.768 kHz crystal, keeping time with the system timer\n");
    }
}

uint32_t time_now(void)
{
    if (state == RUNNING) return rtc_read();
    return soft_base + (jiffies - soft_at) / 1000u;
}

int time_set(uint32_t t)
{
    struct nn_tm tm;
    nn_gmtime(t, &tm);
    if (tm.year < 2000u || tm.year > 2099u) return -1;
    soft_set(t);
    if (state != RUNNING) return 0;
    if (access()) return -1;
    RTC_TSSR = 1u;
    RTC_DWR = tm.wday;
    RTC_CLR = to_bcd(tm.year - 2000u) << 16 | to_bcd(tm.mon) << 8 | to_bcd(tm.mday);
    RTC_TLR = to_bcd(tm.hour) << 16 | to_bcd(tm.min) << 8 | to_bcd(tm.sec);
    return 0;
}
