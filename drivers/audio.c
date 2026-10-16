/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "audio.h"
#include "../arch/nuc140/arch.h"
#include "../arch/nuc140/regs.h"
#include "../kernel/klog.h"
#include "../kernel/kmem.h"
#include "../kernel/sync.h"
#include "../kernel/task.h"
#include "../lib/string.h"
#include "../lib/sys.h"

#define PWMB_BASE  0x40140000u
#define PWM_PPR    REG(PWMB_BASE + 0x00u)
#define PWM_CSR    REG(PWMB_BASE + 0x04u)
#define PWM_PCR    REG(PWMB_BASE + 0x08u)
#define PWM_CNR0   REG(PWMB_BASE + 0x0cu)
#define PWM_CMR0   REG(PWMB_BASE + 0x10u)
#define PWM_POE    REG(PWMB_BASE + 0x7cu)
#define TMR0_TCSR  REG(0x40010000u)
#define TMR0_TCMPR REG(0x40010004u)
#define TMR0_TISR  REG(0x40010008u)

#define PB11       BIT(11)
/* Edge-aligned PWM, CNR 255: the output is low (buzzer on) for CNR - CMR of
 * every 256 clocks, and high (off) all the time when CMR >= CNR. So a sample
 * s turns the buzzer on for s/256 of the time with CMR = 255 - s. */
#define CMR(s)     (255u - (s))

static uint8_t *ring;
static volatile uint32_t head, tail; /* head: written by the task, tail: by the interrupt */
static volatile uint32_t underruns, played;
static volatile uint8_t counting;    /* from the first sample until the close: gaps are underruns */
static struct sem space;             /* given when the ring drains to half */
static volatile uint8_t busy;

void TMR0_IRQHandler(void)
{
    TMR0_TISR = 1u;
    uint32_t t = tail;
    if (t == head) {                         /* keep the last level: no click */
        if (counting) ++underruns;
        return;
    }
    PWM_CMR0 = CMR(ring[t % AUDIO_RING]);
    tail = ++t;
    ++played;
    if (head - t == AUDIO_RING / 2u) sem_give(&space);
}

static void hardware(int on)
{
    uint32_t s = irq_save();
    regs_unlock();
    if (on) {
        CLK_APBCLK |= BIT(22) | BIT(2);                     /* PWM45, TIMER0 */
        CLK_CLKSEL2 = (CLK_CLKSEL2 & ~(BIT(10) | (3u << 4))) | (2u << 4);   /* PWM45 from HCLK */
        CLK_CLKSEL1 = (CLK_CLKSEL1 & ~(7u << 8)) | (2u << 8);              /* TIMER0 from HCLK */
    }
    regs_lock();
    irq_restore(s);
    if (on) {
        PWM_CSR = (PWM_CSR & ~7u) | 4u;                     /* divider 1 */
        PWM_PPR = (PWM_PPR & ~0xffu) | 1u;                  /* prescaler 2: 25.1 MHz */
        PWM_PCR = (PWM_PCR & ~0xfu) | BIT(3);               /* auto-reload, edge-aligned */
        PWM_CNR0 = 255u;                                    /* 98 kHz */
        PWM_CMR0 = CMR(0);                                  /* off */
        PWM_POE |= 1u;
        PWM_PCR |= BIT(0);
        SYS_ALT_MFP |= BIT(4);                              /* PB11: PWM4 */
        SYS_GPB_MFP |= PB11;
        TMR0_TCSR = BIT(26);                                /* reset */
        TMR0_TCMPR = (HCLK_HZ + AUDIO_RATE / 2u) / AUDIO_RATE;
        TMR0_TISR = 1u;
        NVIC_IPR(IRQ_TMR0) &= ~(0xffu << (8 * (IRQ_TMR0 % 4)));   /* highest: steady sample times */
        NVIC_ISER = BIT(IRQ_TMR0);
        TMR0_TCSR = BIT(30) | BIT(29) | (1u << 27);         /* enabled, interrupt, periodic */
    } else {
        TMR0_TCSR = 0;
        NVIC_ICER = BIT(IRQ_TMR0);
        GPIO_DOUT(PORTB) |= PB11;                           /* back to a GPIO, high: off */
        SYS_GPB_MFP &= ~PB11;
        SYS_ALT_MFP &= ~BIT(4);
        PWM_PCR &= ~BIT(0);
        PWM_POE &= ~1u;
        s = irq_save();
        regs_unlock();
        CLK_APBCLK &= ~(BIT(22) | BIT(2));
        regs_lock();
        irq_restore(s);
    }
}

int audio_open(void)
{
    uint32_t s = irq_save();
    int was = busy;
    busy = 1;
    irq_restore(s);
    if (was) return SYS_EBUSY;
    ring = kmalloc(AUDIO_RING);
    if (!ring) { busy = 0; return SYS_ENOMEM; }
    head = tail = underruns = played = 0;
    counting = 0;
    space = (struct sem){ 0 };
    hardware(1);
    return 0;
}

uint32_t audio_write(const uint8_t *p, uint32_t n)
{
    uint32_t done = 0;
    while (done < n) {
        uint32_t h = head, room = AUDIO_RING - (h - tail);
        if (!room) {
            if (task_self()->killed) break;
            sem_take(&space, 20);
            continue;
        }
        uint32_t k = n - done < room ? n - done : room, at = h % AUDIO_RING;
        uint32_t first = AUDIO_RING - at < k ? AUDIO_RING - at : k;
        memcpy(ring + at, p + done, first);
        memcpy(ring, p + done + first, k - first);
        head = h + k;
        done += k;
        counting = 1;
    }
    return done;
}

void audio_close(void)
{
    counting = 0;
    while (head != tail && !task_self()->killed) msleep(10);
    hardware(0);
    kprintf("audio: %u samples played, %u short\n", played, underruns);
    kfree(ring);
    ring = 0;
    busy = 0;
}
