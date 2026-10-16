/* SPDX-License-Identifier: GPL-3.0-or-later */
/* nnnix LDROM bootloader (4 KiB). The chip boots here (CONFIG0 CBS = LDROM,
 * or ISPCON.BS set by nnnix before a reset), with LDROM mapped at 0 and APROM
 * reachable only through ISP commands. It installs a staged update from the
 * SPI flash (update.c), then clears BS and resets into APROM; BS keeps its
 * value through that reset. Runs on the 22.1184 MHz internal oscillator. */
#include "update.h"
#include "../arch/nuc140/regs.h"

#define CLK_AHBCLK  REG(0x50000204u)
#define SYS_IPRSTC1 REG(0x50000008u)
#define FMC_ISPCON  REG(0x5000c000u)
#define FMC_ISPADR  REG(0x5000c004u)
#define FMC_ISPDAT  REG(0x5000c008u)
#define FMC_ISPCMD  REG(0x5000c00cu)
#define FMC_ISPTRG  REG(0x5000c010u)
#define SCB_AIRCR   REG(0xe000ed0cu)
#define UART0       0x40050000u
#define SPI2        0x40130000u
#define ISPCON_ISPEN BIT(0)
#define ISPCON_BS    BIT(1)
#define ISPCON_ISPFF BIT(6)

#ifndef BOOT_VERSION
#define BOOT_VERSION "unknown"
#endif

/* ---- UART0 TX (log on the CP2102) ---- */
static void putc0(char c)
{
    while (UART_FSR(UART0) & UART_FSR_TX_FULL) { }
    UART_DATA(UART0) = (uint8_t)c;
}
static void log_text(const char *s)
{
    for (; *s; ++s) { if (*s == '\n') putc0('\r'); putc0(*s); }
}

/* ---- SPI2: the onboard W25Q16 (CS on PD0) ---- */
static void cs(int on) { if (on) GPIO_DOUT(PORTD) &= ~BIT(0); else GPIO_DOUT(PORTD) |= BIT(0); }
static uint8_t xfer(uint8_t v)
{
    SPI_TX0(SPI2) = v;
    SPI_CNTRL(SPI2) = BIT(2) | (8u << 3) | 1u;
    while (SPI_CNTRL(SPI2) & 1u) { }
    return (uint8_t)SPI_RX0(SPI2);
}
static void command_addr(uint8_t cmd, uint32_t addr)
{
    xfer(cmd); xfer((uint8_t)(addr >> 16)); xfer((uint8_t)(addr >> 8)); xfer((uint8_t)addr);
}
static void spi_read(uint32_t addr, void *buf, uint32_t n)
{
    uint8_t *p = buf;
    cs(1);
    command_addr(0x03, addr);
    while (n--) *p++ = xfer(0);
    cs(0);
}
static int spi_program(uint32_t addr, const void *buf, uint32_t n)   /* within one page */
{
    const uint8_t *p = buf;
    cs(1); xfer(0x06); cs(0);                                        /* write enable */
    cs(1);
    command_addr(0x02, addr);
    while (n--) xfer(*p++);
    cs(0);
    for (uint32_t i = 0; i < 1000000u; ++i) {
        cs(1); xfer(0x05); uint8_t status = xfer(0); cs(0);
        if (!(status & 1u)) return 0;
    }
    return -1;
}

/* ---- APROM through ISP ---- */
static uint32_t isp(uint32_t cmd, uint32_t addr, uint32_t data)
{
    FMC_ISPCMD = cmd;
    FMC_ISPADR = addr;
    FMC_ISPDAT = data;
    FMC_ISPTRG = 1;
    __asm volatile("isb");
    while (FMC_ISPTRG) { }
    return FMC_ISPDAT;
}
static int isp_failed(void)
{
    if (!(FMC_ISPCON & ISPCON_ISPFF)) return 0;
    FMC_ISPCON |= ISPCON_ISPFF;                                     /* write 1 to clear */
    return -1;
}
static uint32_t ap_read(uint32_t addr) { return isp(0x00, addr, 0); }
static int ap_erase(uint32_t page) { isp(0x22, page, 0); return isp_failed(); }
static int ap_write(uint32_t addr, uint32_t word) { isp(0x21, addr, word); return isp_failed(); }

static void init(void)
{
    regs_unlock();
    CLK_PWRCON |= BIT(2);                                           /* HIRC */
    while (!(CLK_CLKSTATUS & BIT(4))) { }
    CLK_CLKSEL0 = (CLK_CLKSEL0 & ~7u) | 7u;                         /* HCLK = HIRC */
    CLK_CLKDIV &= ~(0xfu | (0xfu << 8));
    CLK_CLKSEL1 = (CLK_CLKSEL1 & ~(3u << 24)) | (3u << 24) | BIT(6);  /* UART = HIRC, SPI2 = HCLK */
    CLK_APBCLK |= BIT(16) | BIT(14);                                /* UART0, SPI2 */
    CLK_AHBCLK |= BIT(2);                                           /* flash controller */
    FMC_ISPCON |= ISPCON_ISPEN;                                     /* APROM is writable from LDROM */
    SYS_GPB_MFP |= BIT(1);                                          /* PB1 UART0 TXD */
    UART_FUNSEL(UART0) = 0;
    UART_FCR(UART0) = 6;
    UART_LCR(UART0) = 3;
    UART_BAUD(UART0) = 0x300000beu;                                 /* 115200 from 22.1184 MHz */
    GPIO_DOUT(PORTD) |= BIT(0);
    gpio_mode(PORTD, 0, GPIO_OUTPUT);
    SYS_GPD_MFP |= BIT(1) | BIT(2) | BIT(3);                        /* SPI2 CLK, MISO, MOSI */
    SPI_CNTRL(SPI2) = BIT(2) | (8u << 3);
    SPI_DIVIDER(SPI2) = 1;                                          /* 5.5 MHz */
    SPI_SSR(SPI2) = 0;
    cs(1); xfer(0xab); cs(0);                                       /* release power-down */
    for (volatile int i = 0; i < 200; ++i) { }
}

__attribute__((noreturn)) static void run_aprom(void)
{
    log_text("boot: starting APROM\n");
    while (!(UART_FSR(UART0) & UART_FSR_TE)) { }
    FMC_ISPCON = 0;                                                 /* BS = APROM; ISP off for nnnix */
    SCB_AIRCR = 0x05fa0004u;                                        /* system reset: BS is kept */
    for (;;) { }
}

__attribute__((noreturn)) void boot_main(void)
{
    static const struct boot_hw hw = { spi_read, spi_program, ap_read, ap_erase, ap_write, log_text };
    init();
    log_text("\nboot: nnnix bootloader " BOOT_VERSION "\n");
    if (boot_decide(&hw) == BOOT_RUN_APROM) run_aprom();
    log_text("boot: waiting (program APROM with Nu-Link)\n");
    gpio_mode(PORTC, 12, GPIO_OUTPUT);
    for (;;) {                                                      /* blink the LED (PC12) */
        GPIO_DOUT(PORTC) ^= BIT(12);
        for (volatile uint32_t i = 0; i < 400000u; ++i) { }
    }
}

extern uint32_t __bss_start, __bss_end;
__attribute__((noreturn)) void boot_reset(void)
{
    for (uint32_t *p = &__bss_start; p < &__bss_end; ++p) *p = 0;
    boot_main();
}

static void fault(void) { log_text("boot: fault\n"); for (;;) { } }

__attribute__((section(".vectors"), used)) static const uintptr_t vectors[16] = {
    0x20004000u, (uintptr_t)boot_reset, (uintptr_t)fault, (uintptr_t)fault,
};
