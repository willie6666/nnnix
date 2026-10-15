/* SPDX-License-Identifier: GPL-3.0-or-later */
/* NU-LB_002 Rev 2.1 pin setup for nnnix.
 *
 * PB1  UART0 TX  -> CP2102 RXD (kernel log)
 * PB4  UART1 RX  <- CH9350 TX (receive only)
 * PB5/PB6/PB7 GPIO HIGH: onboard LIN transceiver TXD recessive, NWake inactive,
 *      EN normal -> its RXD (wired to PB4) stays released. Never put LIN to sleep:
 *      the sleep/wake path was measured pulling PB4 low.
 * SPI1 PC9 CLK, PC10 MISO, PC11 MOSI; PC8 CH376S CS; PB10 W5500 CS; PB8 W5500 RST
 * PB14 CH376S INT, PB2 W5500 INT (also sense module power); PC12 LED (active low)
 */
#include "regs.h"
#include "arch.h"

static uint32_t reset_cause;

void board_init(void)
{
    reset_cause = SYS_RSTSRC;
    SYS_RSTSRC = reset_cause;                       /* write 1 to clear */

    GPIO_DOUT(PORTB) |= BIT(5) | BIT(6) | BIT(7);
    gpio_mode(PORTB, 5, GPIO_OUTPUT);
    gpio_mode(PORTB, 6, GPIO_OUTPUT);
    gpio_mode(PORTB, 7, GPIO_OUTPUT);
    GPIO_DOUT(PORTB) |= BIT(4);
    gpio_mode(PORTB, 4, GPIO_QUASI);                /* weak pull-up on CH9350 RX */
    gpio_mode(PORTB, 14, GPIO_INPUT);
    gpio_mode(PORTB, 2, GPIO_INPUT);
    board_spi_pins(0);
    GPIO_DOUT(PORTC) |= BIT(12);
    gpio_mode(PORTC, 12, GPIO_OUTPUT);              /* LED off */

    SYS_GPB_MFP = (SYS_GPB_MFP & ~(BIT(2) | BIT(5) | BIT(6) | BIT(7) | BIT(8) | BIT(10) | BIT(14)))
                | BIT(1) | BIT(4);                  /* PB1 UART0 TXD, PB4 UART1 RXD */
}

int board_modules_powered(void)
{
    /* Both INT lines read low while MB-102 is off (measured); either high means power. */
    uint32_t pins = GPIO_PIN(PORTB);
    return ((pins >> 14) & 1u) || ((pins >> 2) & 1u);
}

void board_spi_pins(int on)
{
    if (on) {
        GPIO_DOUT(PORTC) |= BIT(8);
        GPIO_DOUT(PORTB) |= BIT(10) | BIT(8);
        gpio_mode(PORTC, 8, GPIO_OUTPUT);
        gpio_mode(PORTB, 10, GPIO_OUTPUT);
        gpio_mode(PORTB, 8, GPIO_OUTPUT);
        SYS_GPC_MFP |= BIT(9) | BIT(10) | BIT(11);
        gpio_mode(PORTC, 9, GPIO_OUTPUT);
        gpio_mode(PORTC, 10, GPIO_INPUT);
        gpio_mode(PORTC, 11, GPIO_OUTPUT);
        GPIO_DOUT(PORTB) &= ~BIT(8);                /* W5500 hardware reset */
        delay_ms(2);
        GPIO_DOUT(PORTB) |= BIT(8);
        delay_ms(60);
    } else {
        /* Released while the modules are unpowered so nothing back-powers them. */
        SYS_GPC_MFP &= ~(BIT(9) | BIT(10) | BIT(11));
        for (unsigned pin = 8; pin <= 11; ++pin) gpio_mode(PORTC, pin, GPIO_INPUT);
        gpio_mode(PORTB, 10, GPIO_INPUT);
        gpio_mode(PORTB, 8, GPIO_INPUT);
    }
}

void board_led_toggle(void) { GPIO_DOUT(PORTC) ^= BIT(12); }
uint32_t board_reset_cause(void) { return reset_cause; }

void arch_reboot(void)
{
    __asm volatile("dsb" ::: "memory");
    SCB_AIRCR = 0x05fa0004u;                        /* SYSRESETREQ */
    for (;;) { }
}
