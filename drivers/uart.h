/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NNNIX_UART_H
#define NNNIX_UART_H
#include <stddef.h>
#include <stdint.h>

/* UART0: polled transmit to the CP2102 (kernel log). */
void uart0_init(void);
void uart0_putc(char c);
void uart0_write(const char *s, size_t n);

/* UART1: interrupt-driven receive from the CH9350. */
struct uart1_stats { uint32_t bytes, errors, drops; };
void uart1_init(void);
int  uart1_getc(void);                       /* -1 when empty */
void uart1_set_notify(void (*fn)(void));     /* called from the interrupt when bytes arrive */
void uart1_stats(struct uart1_stats *out);
uint32_t uart1_inject(const uint8_t *p, uint32_t n);   /* tests: bytes as if received; how many fitted */

#endif
