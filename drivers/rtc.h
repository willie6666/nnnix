/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NNNIX_RTC_H
#define NNNIX_RTC_H
#include <stdint.h>

/* Wall clock. The on-chip RTC runs from the 32.768 kHz crystal and keeps time
 * across resets, but not across power loss (the NUC140 has no backup supply).
 * Until the crystal is running, or if it never starts, the time is kept with
 * the system timer instead. */
void     rtc_init(void);             /* start the crystal (boot) */
void     rtc_poll(void);             /* about once per second, from the kernel daemon */
int      rtc_running(void);
uint32_t time_now(void);             /* seconds since 1970-01-01 UTC */
int      time_set(uint32_t t);       /* 0, or -1 outside the RTC's 2000-2099 range */

#endif
