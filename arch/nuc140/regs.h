/* SPDX-License-Identifier: GPL-3.0-or-later */
/* NUC140VE3CN register map (subset used by nnnix).
 * Addresses and bit positions follow the NUC100 series technical reference manual.
 */
#ifndef NNNIX_ARCH_REGS_H
#define NNNIX_ARCH_REGS_H
#include <stdint.h>

#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define BIT(n) (1u << (n))

/* System manager (GCR) */
#define SYS_PDID       REG(0x50000000u)
#define SYS_RSTSRC     REG(0x50000004u)   /* bit0 POR 1 pin 2 WDT 3 LVR 4 BOD 5 SYS 7 CPU */
#define SYS_GPA_MFP    REG(0x50000030u)
#define SYS_GPB_MFP    REG(0x50000034u)
#define SYS_GPC_MFP    REG(0x50000038u)
#define SYS_GPD_MFP    REG(0x5000003cu)
#define SYS_REGWRPROT  REG(0x50000100u)

/* Clock controller */
#define CLK_PWRCON     REG(0x50000200u)
#define CLK_APBCLK     REG(0x50000208u)
#define CLK_CLKSTATUS  REG(0x5000020cu)
#define CLK_CLKSEL0    REG(0x50000210u)
#define CLK_CLKSEL1    REG(0x50000214u)
#define CLK_CLKDIV     REG(0x50000218u)
#define CLK_PLLCON     REG(0x50000220u)

/* RTC (clocked by the 32.768 kHz crystal LXT) */
#define RTC_INIR       REG(0x40008000u)   /* bit0: active */
#define RTC_AER        REG(0x40008004u)   /* bit16: write access enabled */
#define RTC_TLR        REG(0x4000800cu)   /* BCD time */
#define RTC_CLR        REG(0x40008010u)   /* BCD date, year from 2000 */
#define RTC_TSSR       REG(0x40008014u)   /* bit0: 24-hour */
#define RTC_DWR        REG(0x40008018u)   /* day of the week */

/* GPIO: port n at 0x50004000 + 0x40 * n */
enum { PORTA, PORTB, PORTC, PORTD, PORTE };
#define GPIO_PMD(p)    REG(0x50004000u + 0x40u * (p))   /* 2 bits/pin: 0 in 1 out 2 OD 3 quasi */
#define GPIO_DOUT(p)   REG(0x50004008u + 0x40u * (p))
#define GPIO_PIN(p)    REG(0x50004010u + 0x40u * (p))
#define GPIO_IMD(p)    REG(0x50004018u + 0x40u * (p))   /* 0 edge, 1 level */
#define GPIO_IEN(p)    REG(0x5000401cu + 0x40u * (p))   /* [15:0] falling/low, [31:16] rising/high */
#define GPIO_ISRC(p)   REG(0x50004020u + 0x40u * (p))   /* write 1 to clear */
enum { GPIO_INPUT, GPIO_OUTPUT, GPIO_OPEN_DRAIN, GPIO_QUASI };

/* UART (UA_RBR/THR 0x00, IER 0x04, FCR 0x08, LCR 0x0C, FSR 0x18, BAUD 0x24, FUN_SEL 0x30) */
#define UART0_BASE     0x40050000u
#define UART1_BASE     0x40150000u
#define UART_DATA(b)   REG((b) + 0x00u)
#define UART_IER(b)    REG((b) + 0x04u)
#define UART_FCR(b)    REG((b) + 0x08u)
#define UART_LCR(b)    REG((b) + 0x0cu)
#define UART_FSR(b)    REG((b) + 0x18u)
#define UART_BAUD(b)   REG((b) + 0x24u)
#define UART_FUNSEL(b) REG((b) + 0x30u)
#define UART_FSR_ERRORS   0x71u        /* RX overflow, parity, framing, break */
#define UART_FSR_RX_EMPTY BIT(14)
#define UART_FSR_TX_FULL  BIT(23)
#define UART_FSR_TE       BIT(28)      /* transmitter empty */

/* SPI1 (CNTRL 0x00, DIVIDER 0x04, SSR 0x08, RX0 0x10, TX0 0x20) */
#define SPI1_BASE      0x40034000u
#define SPI_CNTRL(b)   REG((b) + 0x00u)
#define SPI_DIVIDER(b) REG((b) + 0x04u)
#define SPI_SSR(b)     REG((b) + 0x08u)
#define SPI_RX0(b)     REG((b) + 0x10u)
#define SPI_TX0(b)     REG((b) + 0x20u)

#define WDT_WTCR       REG(0x40004000u)

/* Cortex-M0 system control space */
#define SYST_CSR       REG(0xe000e010u)
#define SYST_RVR       REG(0xe000e014u)
#define SYST_CVR       REG(0xe000e018u)
#define NVIC_ISER      REG(0xe000e100u)
#define NVIC_ICER      REG(0xe000e180u)
#define SCB_ICSR       REG(0xe000ed04u)   /* bit 28 PENDSVSET */
#define SCB_AIRCR      REG(0xe000ed0cu)
#define SCB_SHPR2      REG(0xe000ed1cu)   /* [31:24] SVCall priority */
#define SCB_SHPR3      REG(0xe000ed20u)   /* [23:16] PendSV, [31:24] SysTick */
#define NVIC_IPR(n)    REG(0xe000e400u + 4u * ((n) / 4u))

#define IRQ_GPAB       4
#define IRQ_UART1      13

static inline void regs_unlock(void)
{
    SYS_REGWRPROT = 0x59; SYS_REGWRPROT = 0x16; SYS_REGWRPROT = 0x88;
}
static inline void regs_lock(void) { SYS_REGWRPROT = 0; }

static inline void gpio_mode(unsigned port, unsigned pin, unsigned mode)
{
    GPIO_PMD(port) = (GPIO_PMD(port) & ~(3u << (2u * pin))) | (mode << (2u * pin));
}

#endif
