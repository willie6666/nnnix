/* 50.19 MHz system clock from the internal 22.1184 MHz oscillator, 1 ms SysTick. */
#include "regs.h"
#include "arch.h"

volatile uint32_t jiffies;

void SysTick_Handler(void) { jiffies++; }

void clock_init(void)
{
    regs_unlock();
    WDT_WTCR = 0;                                   /* watchdog off until M7 */
    CLK_PWRCON |= BIT(2);                           /* HIRC on */
    while (!(CLK_CLKSTATUS & BIT(4))) { }
    CLK_CLKSEL0 = (CLK_CLKSEL0 & ~7u) | 7u;          /* HCLK = HIRC while the PLL starts */
    /* PLL from HIRC, NR 13, NF 59, NO 2 (BSP CLK_PLLCON_50MHz_HIRC). */
    CLK_PLLCON = BIT(19) | ((13u - 2u) << 9) | (59u - 2u) | 0x4000u;
    while (!(CLK_CLKSTATUS & BIT(2))) { }
    CLK_CLKDIV &= ~(0xfu | (0xfu << 8));            /* HCLK_N = UART_N = 0 */
    CLK_CLKSEL0 = (CLK_CLKSEL0 & ~7u) | 2u;          /* HCLK = PLL */
    CLK_CLKSEL1 = (CLK_CLKSEL1 & ~(3u << 24)) | (3u << 24) | BIT(5);  /* UART = HIRC, SPI1 = HCLK */
    CLK_APBCLK |= BIT(16) | BIT(17) | BIT(13);      /* UART0, UART1, SPI1 */
    regs_lock();

    SYST_CSR = 0;
    SYST_RVR = TICKS_PER_MS - 1u;
    SYST_CVR = 0;
    SYST_CSR = 7;                                   /* core clock, interrupt, enable */
}

void delay_ms(uint32_t ms)
{
    uint32_t start = jiffies;
    while (jiffies - start < ms) { }
}

void delay_us(uint32_t us)
{
    uint32_t need = us * (HCLK_HZ / 1000000u), elapsed = 0, prev = SYST_CVR;
    while (elapsed < need) {
        uint32_t now = SYST_CVR;
        elapsed += prev >= now ? prev - now : prev + TICKS_PER_MS - now;
        prev = now;
    }
}
