/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Architecture interface: clock, time, board pins, faults. */
#ifndef NNNIX_ARCH_H
#define NNNIX_ARCH_H
#include <stdint.h>

#define HCLK_HZ      50191800u   /* 22.1184 MHz HIRC * 59 / 13 / 2 */
#define TICKS_PER_MS 50192u

extern volatile uint32_t jiffies;    /* milliseconds since boot */

void clock_init(void);
void delay_ms(uint32_t ms);
void delay_us(uint32_t us);

void board_init(void);
int  board_modules_powered(void);    /* MB-102 rail sensed through CH376S/W5500 INT lines */
void board_spi_pins(int on);
void board_w5500_reset(int asserted);   /* PB8 */
void board_led_toggle(void);
void board_led_set(int on);         /* PC12, active low */
void board_w5500_irq(int enable);   /* PB2 falling edge -> w5500_isr() */
uint32_t board_reset_cause(void);    /* SYS_RSTSRC captured at boot */

void fault_report_last(void);        /* print the crash record left by a previous HardFault */
const char *fault_last_report(void); /* that report as text, "" if the last boot was clean */
void fault_trigger(void) __attribute__((noreturn));
void arch_reboot(void) __attribute__((noreturn));
void arch_reboot_bootloader(void) __attribute__((noreturn));   /* through LDROM (fmc.c) */
void arch_reboot_chip(void) __attribute__((noreturn));         /* like power-on */
uint32_t fmc_read(uint32_t addr);                              /* LDROM 0x100000, CONFIG 0x300000 */
int  wdt_set(int seconds);           /* watchdog: 0 off, 2, 7 or 26 s; -1 for other values (wdt.c) */
int  wdt_seconds(void);
void wdt_kick(void);

static inline void irq_disable(void) { __asm volatile("cpsid i" ::: "memory"); }
static inline void irq_enable(void) { __asm volatile("cpsie i" ::: "memory"); }

/* Nestable critical section: returns the previous PRIMASK. */
static inline uint32_t irq_save(void)
{
    uint32_t primask;
    __asm volatile("mrs %0, primask\n cpsid i" : "=r"(primask) :: "memory");
    return primask;
}
static inline void irq_restore(uint32_t primask)
{
    __asm volatile("msr primask, %0" :: "r"(primask) : "memory");
}
static inline int in_interrupt(void)
{
    uint32_t ipsr;
    __asm volatile("mrs %0, ipsr" : "=r"(ipsr));
    return ipsr != 0;
}

/* Context switching (switch.c). Tasks run in thread mode on PSP; handlers use MSP. */
void arch_sched_init(void);                       /* exception priorities */
void arch_pend_switch(void);                      /* request PendSV */
void arch_start_first_task(uint32_t *sp) __attribute__((noreturn));
uint32_t *arch_task_stack_init(uint32_t *top, void (*entry)(void *), void *arg, void (*exit)(void));

/* Record a kernel panic like a fault (message kept across reboot), then reboot. */
void arch_panic(const char *msg, uint32_t pc) __attribute__((noreturn));

#endif
