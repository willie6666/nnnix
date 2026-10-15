/* SPDX-License-Identifier: GPL-3.0-or-later */
/* nnnix kernel entry: hardware init, then the system tasks under the scheduler. */
#include "kernel.h"
#include "klog.h"
#include "kmem.h"
#include "task.h"
#include "../arch/nuc140/arch.h"
#include "../arch/nuc140/regs.h"
#include "../drivers/uart.h"
#include "../drivers/spi.h"
#include "../drivers/spiflash.h"
#include "console.h"
#include "kd.h"
#include "../fs/vfs.h"
#include "panic.h"
#include "../net/telnetd.h"

#ifndef NNNIX_VERSION
#define NNNIX_VERSION "unknown"
#endif
const char nnnix_version[] = NNNIX_VERSION;

#define STACK_FILL 0x5354434bu        /* "STCK" */
extern uint32_t __data_start, __bss_end, __noinit_base, __noinit_end, __stack_limit, __stack_top;
extern uint32_t __heap_start, __heap_end;

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

void kmain(void)
{
    clock_init();
    board_init();
    uart0_init();
    stack_paint();

    kprintf("\nnnnix %s NUC140VE3CN pdid 0x%08x hclk %u Hz\n", nnnix_version, SYS_PDID, HCLK_HZ);
    uint32_t cause = board_reset_cause();
    kprintf("reset cause 0x%02x:", cause);
    for (int i = 0; i < 8; ++i) if ((cause >> i) & 1u && reset_names[i]) kprintf(" %s", reset_names[i]);
    kprintf("\n");
    fault_report_last();

    kmem_init(&__heap_start, &__heap_end);
    uart1_init();
    spi_init();
    sched_init();
    telnetd_init();
    console_init();
    spiflash_init();
    kprintf("spiflash: jedec id %06x, %u KiB\n", spiflash_jedec_id(), spiflash_size() / 1024u);
    vfs_init();
    kd_init();
    struct kmem_stats m;
    kmem_stats(&m);
    kprintf("ram: static %u B, msp %u B, heap %u B (%u B free after tasks)\n",
            ram_static(), stack_size(), m.total, m.free);
    sched_start();
}
