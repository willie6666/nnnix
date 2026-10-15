/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "uart.h"
#include "../arch/nuc140/regs.h"
#include "../kernel/sync.h"

/* UART clock is HIRC 22.1184 MHz: DIV_X_EN | DIV_X_ONE, BRD 190 -> 115200 baud. */
#define BAUD_115200 0x300000beu
#define RX_RING 256u

static void setup(uint32_t base)
{
    UART_IER(base) = 0;
    UART_FUNSEL(base) = 0;
    UART_FCR(base) = 6;              /* reset RX and TX FIFOs */
    UART_LCR(base) = 3;              /* 8N1 */
    UART_BAUD(base) = BAUD_115200;
}

void uart0_init(void) { setup(UART0_BASE); }

void uart0_putc(char c)
{
    while (UART_FSR(UART0_BASE) & UART_FSR_TX_FULL) { }
    UART_DATA(UART0_BASE) = (uint8_t)c;
}

void uart0_write(const char *s, size_t n)
{
    while (n--) {
        if (*s == '\n') uart0_putc('\r');
        uart0_putc(*s++);
    }
}

static uint8_t ring[RX_RING];
static volatile uint32_t head;
static uint32_t tail;
static volatile struct uart1_stats stats;
static struct sem rx_ready;

void UART1_IRQHandler(void)
{
    uint32_t flags = UART_FSR(UART1_BASE);
    if (flags & UART_FSR_ERRORS) {
        stats.errors++;
        UART_FSR(UART1_BASE) = flags & UART_FSR_ERRORS;
    }
    while (!(UART_FSR(UART1_BASE) & UART_FSR_RX_EMPTY)) {
        uint8_t byte = (uint8_t)UART_DATA(UART1_BASE);
        stats.bytes++;
        if (head - tail < RX_RING) { ring[head % RX_RING] = byte; head = head + 1u; }
        else stats.drops++;
    }
    if (rx_ready.count <= 0) sem_give(&rx_ready);
}

int uart1_wait(uint32_t timeout_ms)
{
    if (tail != head) return 0;
    int r = sem_take(&rx_ready, timeout_ms);
    rx_ready.count = 0;              /* one wakeup drains everything that arrived */
    return r;
}

void uart1_init(void)
{
    setup(UART1_BASE);
    UART_IER(UART1_BASE) = BIT(0);   /* receive data available */
    NVIC_ISER = BIT(IRQ_UART1);
}

int uart1_getc(void)
{
    if (tail == head) return -1;
    return ring[tail++ % RX_RING];
}

void uart1_stats(struct uart1_stats *out)
{
    out->bytes = stats.bytes;
    out->errors = stats.errors;
    out->drops = stats.drops;
}
