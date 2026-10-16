/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Reset entry, vector table and C runtime setup. */
#include <stdint.h>

extern uint32_t __data_load, __data_start, __data_end, __bss_start, __bss_end;
extern unsigned char __boot_stack_top;
void kmain(void);

void Reset_Handler(void)
{
    uint32_t *src = &__data_load, *dst = &__data_start;
    while (dst < &__data_end) *dst++ = *src++;
    for (dst = &__bss_start; dst < &__bss_end; ) *dst++ = 0;
    kmain();
    for (;;) { }
}

void Default_Handler(void) { for (;;) { } }
void HardFault_Handler(void);
void SVC_Handler(void);
void PendSV_Handler(void);
void SysTick_Handler(void);
void GPAB_IRQHandler(void);
void UART1_IRQHandler(void);
void TMR0_IRQHandler(void);

#define DEFAULT (uintptr_t)Default_Handler
__attribute__((section(".vectors"), used)) const uintptr_t vectors[48] = {
    (uintptr_t)&__boot_stack_top,         /* see link.ld */
    (uintptr_t)Reset_Handler,
    DEFAULT,                          /* NMI */
    (uintptr_t)HardFault_Handler,
    [4 ... 10] = DEFAULT,
    [11] = (uintptr_t)SVC_Handler,
    [12 ... 13] = DEFAULT,
    [14] = (uintptr_t)PendSV_Handler,
    [15] = (uintptr_t)SysTick_Handler,
    [16 ... 19] = DEFAULT,
    [16 + 4] = (uintptr_t)GPAB_IRQHandler,
    [21 ... 23] = DEFAULT,
    [16 + 8] = (uintptr_t)TMR0_IRQHandler,
    [25 ... 28] = DEFAULT,
    [16 + 13] = (uintptr_t)UART1_IRQHandler,
    [30 ... 47] = DEFAULT,
};
