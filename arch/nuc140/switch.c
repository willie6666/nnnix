/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Cortex-M0 context switch and system-call entry.
 *
 * Task stack after a switch-out (low to high address):
 *   r4 r5 r6 r7 r8 r9 r10 r11 | r0 r1 r2 r3 r12 lr pc xpsr   (hardware frame)
 * struct task's first field holds the saved stack pointer.
 */
#include "regs.h"
#include "arch.h"

extern void *sched_current;          /* struct task *, first field uint32_t *sp */
void sched_switch(void);             /* picks the next task, updates sched_current */
void svc_redirect(uint32_t *frame);  /* kernel/syscall.c */

void arch_sched_init(void)
{
    /* Two priority bits on M0. PendSV lowest so it never preempts a handler. */
    SCB_SHPR2 = (SCB_SHPR2 & 0x00ffffffu) | (0x80u << 24);              /* SVCall */
    SCB_SHPR3 = (SCB_SHPR3 & 0x0000ffffu) | (0xc0u << 16) | (0x80u << 24); /* PendSV, SysTick */
    NVIC_IPR(IRQ_UART1) = (NVIC_IPR(IRQ_UART1) & ~(0xffu << (8 * (IRQ_UART1 % 4))))
                        | (0x40u << (8 * (IRQ_UART1 % 4)));
}

void arch_pend_switch(void) { SCB_ICSR = BIT(28); }

uint32_t *arch_task_stack_init(uint32_t *top, void (*entry)(void *), void *arg, void (*exit)(void))
{
    uint32_t *sp = (uint32_t *)((uintptr_t)top & ~7u);
    *--sp = 0x01000000u;                         /* xPSR: Thumb */
    *--sp = (uint32_t)(uintptr_t)entry & ~1u;    /* pc */
    *--sp = (uint32_t)(uintptr_t)exit;           /* lr: task returns into exit */
    *--sp = 0;                                   /* r12 */
    *--sp = 0; *--sp = 0; *--sp = 0;             /* r3 r2 r1 */
    *--sp = (uint32_t)(uintptr_t)arg;            /* r0 */
    for (int i = 0; i < 8; ++i) *--sp = 0;       /* r11..r4 */
    return sp;
}

__attribute__((naked)) int arch_call_sb(int (*fn)(int, char **, const void *), int argc, char **argv, void *sb)
{
    __asm volatile(
        "push  {r4, lr}\n"
        "mov   r4, r9\n"
        "mov   r9, r3\n"
        "mov   r3, r0\n"
        "mov   r0, r1\n"
        "mov   r1, r2\n"
        "ldr   r2, =nn_lib\n"
        "blx   r3\n"
        "mov   r9, r4\n"
        "pop   {r4, pc}\n"
        ".ltorg\n");
}

__attribute__((naked)) void PendSV_Handler(void)
{
    __asm volatile(
        "mrs   r0, psp\n"
        "subs  r0, #32\n"
        "ldr   r1, =sched_current\n"
        "ldr   r2, [r1]\n"
        "str   r0, [r2]\n"              /* current->sp */
        "stmia r0!, {r4-r7}\n"
        "mov   r4, r8\n"
        "mov   r5, r9\n"
        "mov   r6, r10\n"
        "mov   r7, r11\n"
        "stmia r0!, {r4-r7}\n"
        "push  {r3, lr}\n"
        "bl    sched_switch\n"
        "pop   {r2, r3}\n"
        "mov   lr, r3\n"
        "ldr   r1, =sched_current\n"
        "ldr   r2, [r1]\n"
        "ldr   r0, [r2]\n"              /* next->sp */
        "adds  r0, #16\n"
        "ldmia r0!, {r4-r7}\n"
        "mov   r8, r4\n"
        "mov   r9, r5\n"
        "mov   r10, r6\n"
        "mov   r11, r7\n"
        "msr   psp, r0\n"
        "subs  r0, #32\n"
        "ldmia r0!, {r4-r7}\n"
        "bx    lr\n");
}

/* System calls: the handler only rewrites the stacked frame so the kernel
 * function runs in the calling task's own context (it may block), then returns
 * straight to the instruction after the svc with its result in r0. */
__attribute__((naked)) void SVC_Handler(void)
{
    __asm volatile(
        "movs  r0, #4\n"
        "mov   r1, lr\n"
        "tst   r0, r1\n"
        "beq   1f\n"
        "mrs   r0, psp\n"
        "b     2f\n"
        "1:\n"
        "mrs   r0, msp\n"
        "2:\n"
        "ldr   r1, =svc_redirect\n"
        "bx    r1\n");
}

/* Leave kmain for good: MSP restarts at the top for handlers, the task runs on PSP. */
__attribute__((naked, noreturn)) void arch_start_first_task(uint32_t *sp)
{
    __asm volatile(
        "adds  r0, #32\n"               /* skip r4-r11 (zero for a new task) */
        "msr   psp, r0\n"
        "ldr   r1, =__stack_top\n"
        "msr   msp, r1\n"
        "movs  r1, #2\n"
        "msr   control, r1\n"           /* thread mode uses PSP */
        "isb\n"
        "pop   {r0-r5}\n"               /* r0-r3, r12 -> r4, lr -> r5 */
        "mov   r12, r4\n"
        "mov   lr, r5\n"
        "pop   {r4, r5}\n"              /* pc, xpsr */
        "movs  r5, #1\n"
        "orrs  r4, r5\n"
        "cpsie i\n"
        "bx    r4\n");
}
