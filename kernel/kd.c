/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "kd.h"
#include "console.h"
#include "diag.h"
#include "input.h"
#include "kernel.h"
#include "klog.h"
#include "modules.h"
#include "panic.h"
#include "sync.h"
#include "task.h"
#include "../arch/nuc140/arch.h"
#include "../drivers/spi.h"
#include "../drivers/uart.h"
#include "../drivers/w5500.h"
#include "../net/telnetd.h"
#include "../net/http.h"
#include "../user/nsh.h"

#define POLL_MS 50u

static struct sem wake;

void kd_notify(void) { if (wake.count <= 0) sem_give(&wake); }

static int due(uint32_t at) { return (int32_t)(jiffies - at) >= 0; }

static uint32_t until(uint32_t at)
{
    int32_t left = (int32_t)(at - jiffies);
    return left > 0 ? (uint32_t)left : 0;
}

static void statistics(void)
{
    struct uart1_stats u;
    uart1_stats(&u);
    kprintf("stat: uart1 %u B err %u drop %u, ch9350 bad %u, mouse %u, spi timeouts %u, msp %u B\n",
            u.bytes, u.errors, u.drops, input_bad_bytes(), input_mouse_reports(), spi_timeouts(), stack_used());
}

static void kd_main(void *arg)
{
    (void)arg;
    uint32_t poll_at = jiffies, second_at = jiffies, minute_at = jiffies + 60000u;
    for (;;) {
        uint32_t timeout = until(poll_at), repeat = input_next_ms();
        if (until(second_at) < timeout) timeout = until(second_at);
        if (repeat < timeout) timeout = repeat;
        if (timeout) sem_take(&wake, timeout);

        input_poll();
        if (modules.w5500) w5500_collect();
        if (due(poll_at)) {
            poll_at = jiffies + POLL_MS;
            if (modules.w5500) {
                diag_poll();
                httpd_poll();
                if (nsh_boot_done()) telnetd_poll();
            }
            if (nsh_boot_done()) console_poll();
        }
        if (due(second_at)) {
            second_at += 1000u;
            modules_poll();
            board_led_toggle();
        }
        if (due(minute_at)) {
            minute_at += 60000u;
            statistics();
        }
    }
}

void kd_init(void)
{
    uart1_set_notify(kd_notify);
    w5500_set_notify(kd_notify);
    if (!task_spawn("kd", kd_main, 0, 1024, 2, 1)) panic("cannot start kd");
}
