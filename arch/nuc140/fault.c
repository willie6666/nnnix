/* SPDX-License-Identifier: GPL-3.0-or-later */
/* HardFault capture: the stacked registers are saved in RAM that survives a
 * system reset (.noinit), dumped to UART0, and the board reboots. The next boot
 * reports the record through klog.
 */
#include "regs.h"
#include "arch.h"
#include "../../kernel/klog.h"

#define CRASH_MAGIC 0x4352534au      /* "CRSJ" */

struct crash {
    uint32_t magic, count;
    uint32_t r0, r1, r2, r3, r12, lr, pc, xpsr, sp, exc_return, jiffies;
    uint32_t check;                 /* ~(magic ^ pc ^ count) */
    char panic[48];                 /* empty for a HardFault */
} __attribute__((aligned(4)));
__attribute__((section(".noinit"))) static struct crash crash;
__attribute__((section(".noinit"), used)) static uint32_t fault_stack[64];

static void raw_putc(char c)
{
    while (UART_FSR(UART0_BASE) & UART_FSR_TX_FULL) { }
    UART_DATA(UART0_BASE) = (uint8_t)c;
}
static void raw_puts(const char *s) { while (*s) raw_putc(*s++); }
static void raw_hex(const char *label, uint32_t v)
{
    raw_puts(label);
    for (int i = 28; i >= 0; i -= 4) raw_putc("0123456789abcdef"[(v >> i) & 15u]);
}

__attribute__((used)) void fault_handler_c(const uint32_t *frame, uint32_t exc_return)
{
    uint32_t count = crash.magic == CRASH_MAGIC ? crash.count + 1u : 1u;
    crash = (struct crash){ CRASH_MAGIC, count, 0, 0, 0, 0, 0, 0, 0, 0,
                            (uint32_t)(uintptr_t)frame, exc_return, jiffies, 0, { 0 } };
    /* Only trust the frame when it points into SRAM. */
    if ((uintptr_t)frame >= 0x20000000u && (uintptr_t)frame <= 0x20004000u - 32u) {
        crash.r0 = frame[0]; crash.r1 = frame[1]; crash.r2 = frame[2]; crash.r3 = frame[3];
        crash.r12 = frame[4]; crash.lr = frame[5]; crash.pc = frame[6]; crash.xpsr = frame[7];
    }
    crash.check = ~(crash.magic ^ crash.pc ^ crash.count);
    raw_puts("\r\n*** HardFault");
    raw_hex(" pc=", crash.pc); raw_hex(" lr=", crash.lr); raw_hex(" sp=", crash.sp);
    raw_hex(" xpsr=", crash.xpsr); raw_hex(" exc=", exc_return);
    raw_puts("\r\n*** rebooting\r\n");
    while (!(UART_FSR(UART0_BASE) & UART_FSR_TE)) { }
    arch_reboot();
}

/* Pick the stack the exception frame was pushed on, then switch to a private
 * stack so a smashed main stack cannot take the handler down with it. */
__attribute__((naked)) void HardFault_Handler(void)
{
    __asm volatile(
        "movs r0, #4\n"
        "mov  r1, lr\n"
        "tst  r0, r1\n"
        "beq  1f\n"
        "mrs  r0, psp\n"
        "b    2f\n"
        "1:\n"
        "mrs  r0, msp\n"
        "2:\n"
        "ldr  r2, =fault_stack + 256\n"
        "mov  sp, r2\n"
        "ldr  r2, =fault_handler_c\n"
        "bx   r2\n");
}

void arch_panic(const char *msg, uint32_t pc)
{
    irq_disable();
    uint32_t sp;
    __asm volatile("mov %0, sp" : "=r"(sp));
    uint32_t count = crash.magic == CRASH_MAGIC ? crash.count + 1u : 1u;
    crash = (struct crash){ CRASH_MAGIC, count, 0, 0, 0, 0, 0, 0, pc, 0, sp, 0, jiffies, 0, { 0 } };
    for (unsigned i = 0; i + 1 < sizeof crash.panic && msg[i]; ++i) crash.panic[i] = msg[i];
    crash.check = ~(crash.magic ^ crash.pc ^ crash.count);
    raw_puts("\r\n*** PANIC: ");
    raw_puts(crash.panic);
    raw_hex(" pc=", pc);
    raw_puts("\r\n*** rebooting\r\n");
    while (!(UART_FSR(UART0_BASE) & UART_FSR_TE)) { }
    arch_reboot();
}

void fault_report_last(void)
{
    if (crash.magic != CRASH_MAGIC || crash.check != ~(crash.magic ^ crash.pc ^ crash.count)) {
        crash.magic = 0;
        return;
    }
    crash.panic[sizeof crash.panic - 1] = 0;
    if (crash.panic[0])
        kprintf("last crash #%u: panic: %s (pc=%08x sp=%08x at %u ms)\n",
                crash.count, crash.panic, crash.pc, crash.sp, crash.jiffies);
    else
    kprintf("last crash #%u: pc=%08x lr=%08x sp=%08x xpsr=%08x r0=%08x r1=%08x at %u ms\n",
            crash.count, crash.pc, crash.lr, crash.sp, crash.xpsr, crash.r0, crash.r1, crash.jiffies);
    crash.magic = 0;     /* report once; count restarts at the next fault */
}

void fault_trigger(void)
{
    __asm volatile("udf #0");
    for (;;) { }
}
