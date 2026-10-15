/* nnnix M0: single-threaded bring-up loop. The scheduler arrives in M1. */
#include "kernel.h"
#include "klog.h"
#include "diag.h"
#include "modules.h"
#include "../arch/nuc140/arch.h"
#include "../arch/nuc140/regs.h"
#include "../drivers/uart.h"
#include "../drivers/spi.h"
#include "../drivers/ch9350.h"
#include "../drivers/hidkey.h"

#ifndef NNNIX_VERSION
#define NNNIX_VERSION "unknown"
#endif
const char nnnix_version[] = NNNIX_VERSION;

#define STACK_FILL 0x5354434bu        /* "STCK" */
extern uint32_t __data_start, __bss_end, __noinit_base, __noinit_end, __stack_limit, __stack_top;

static void stack_paint(void)
{
    uint32_t sp;
    __asm volatile("mov %0, sp" : "=r"(sp));
    for (uint32_t *p = &__stack_limit; (uintptr_t)p < sp - 64u; ++p) *p = STACK_FILL;
}

uint32_t stack_used(void)
{
    const uint32_t *p = &__stack_limit;
    while (p < &__stack_top && *p == STACK_FILL) ++p;
    return (uint32_t)((uintptr_t)&__stack_top - (uintptr_t)p);
}
uint32_t stack_size(void) { return (uint32_t)((uintptr_t)&__stack_top - (uintptr_t)&__stack_limit); }
uint32_t ram_static(void)
{
    return (uint32_t)((uintptr_t)&__bss_end - (uintptr_t)&__data_start)
         + (uint32_t)((uintptr_t)&__noinit_end - (uintptr_t)&__noinit_base);
}

static const char *const reset_names[8] = { "power-on", "reset-pin", "watchdog", "low-voltage",
                                            "brown-out", "system", 0, "cpu" };

static struct ch9350_parser parser;
static uint8_t keys[8];

static int contains(const uint8_t *report, uint8_t usage)
{
    for (int i = 2; i < 8; ++i) if (report[i] == usage) return 1;
    return 0;
}

static void key_log(const char *what, uint8_t usage)
{
    const char *name = hid_key_name(usage);
    if (name) kprintf("key: %s %s\n", what, name);
    else kprintf("key: %s 0x%02x\n", what, usage);
}

static void input_event(const struct ch9350_event *ev)
{
    switch (ev->type) {
    case CH9350_KEYBOARD:
        if (ev->data[0] != keys[0]) kprintf("key: modifiers 0x%02x\n", ev->data[0]);
        for (int i = 2; i < 8; ++i) {
            uint8_t u = ev->data[i];
            if (u > 1 && !contains(keys, u)) {
                key_log("down", u);
                if (u == 0x3a && modules.ch376) diag_ch376_bench(1ul << 20);   /* F1 */
            }
        }
        for (int i = 2; i < 8; ++i)
            if (keys[i] > 1 && !contains(ev->data, keys[i])) key_log("up", keys[i]);
        for (int i = 0; i < 8; ++i) keys[i] = ev->data[i];
        break;
    case CH9350_MOUSE:
        kprintf("mouse: btn %x dx %d dy %d wheel %d\n", ev->data[0],
                (int8_t)ev->data[1], (int8_t)ev->data[2], (int8_t)ev->data[3]);
        break;
    case CH9350_STATUS:
        break;
    case CH9350_REMOVED:
        kprintf("ch9350: device removed\n");
        for (int i = 0; i < 8; ++i) keys[i] = 0;
        break;
    }
}

void kmain(void)
{
    clock_init();
    board_init();
    uart0_init();
    stack_paint();

    kprintf("\nnnnix %s (M0) NUC140VE3CN pdid 0x%08x hclk %u Hz\n", nnnix_version, SYS_PDID, HCLK_HZ);
    uint32_t cause = board_reset_cause();
    kprintf("reset cause 0x%02x:", cause);
    for (int i = 0; i < 8; ++i) if ((cause >> i) & 1u && reset_names[i]) kprintf(" %s", reset_names[i]);
    kprintf("\n");
    fault_report_last();
    kprintf("ram: static %u B, stack %u B, total 16384 B\n", ram_static(), stack_size());

    uart1_init();
    spi_init();
    modules_poll();

    uint32_t second = jiffies, minute = jiffies, polled = jiffies;
    for (;;) {
        int c;
        struct ch9350_event ev;
        while ((c = uart1_getc()) >= 0)
            if (ch9350_feed(&parser, (uint8_t)c, &ev)) input_event(&ev);
        if (modules.w5500 && jiffies != polled) { polled = jiffies; diag_poll(); }
        if (jiffies - second >= 1000u) {
            second = jiffies;
            board_led_toggle();
            modules_poll();
        }
        if (jiffies - minute >= 60000u) {
            struct uart1_stats u;
            minute = jiffies;
            uart1_stats(&u);
            kprintf("stat: uart1 %u B err %u drop %u, ch9350 bad %u, spi timeouts %u, stack %u B\n",
                    u.bytes, u.errors, u.drops, parser.bad_bytes, spi_timeouts(), stack_used());
        }
    }
}
