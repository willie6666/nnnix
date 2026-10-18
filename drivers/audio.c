/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "audio.h"
#include "audioring.h"
#include "../arch/nuc140/arch.h"
#include "../arch/nuc140/regs.h"
#include "../kernel/klog.h"
#include "../kernel/kmem.h"
#include "../kernel/panic.h"
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
#define TMR0_TDR   REG(0x4001000cu)

#define PB11       BIT(11)
/* Edge-aligned PWM, CNR 255: the output is low (buzzer on) for CNR - CMR of
 * every 256 clocks, and high (off) all the time when CMR >= CNR. So a sample
 * s turns the buzzer on for s/256 of the time with CMR = 255 - s. */
#define CMR(s)     (255u - (s))

static struct audio_ring ring;
static volatile uint32_t underruns, played;
static volatile uint32_t late_max;   /* the timer's count at the interrupt: its latency, in HCLK cycles */
static volatile uint32_t took_max;   /* and at its end: how long it ran */
static volatile uint8_t counting;    /* from the first sample until the close: gaps are underruns */
static volatile uint8_t waiting;     /* a writer waits for room: given space when wake bytes are free */
static uint32_t wake;
static struct sem space;
static volatile uint8_t busy;

void TMR0_IRQHandler(void)
{
    uint32_t late = TMR0_TDR;        /* counts up from 0 at each tick: how long interrupts were held off */
    if (late > late_max) late_max = late;
    TMR0_TISR = 1u;
    int s = ar_next(&ring);
    if (s >= 0) {
        PWM_CMR0 = CMR((uint32_t)s);
        ++played;
    } else if (counting) {
        ++underruns;                 /* keep the last level: no click */
    }
    if (waiting && ring.size - ar_used(&ring) >= wake) {
        waiting = 0;
        sem_give(&space);
    }
    uint32_t took = TMR0_TDR - late;
    if (took > took_max) took_max = took;
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
        TMR0_TCSR = BIT(30) | BIT(29) | (1u << 27) | BIT(16);   /* enabled, interrupt, periodic, TDR readable */
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

int audio_open(int adpcm)
{
    uint32_t s = irq_save();
    int was = busy;
    busy = 1;
    irq_restore(s);
    if (was) return SYS_EBUSY;
    uint32_t size = AUDIO_RING;
    uint8_t *buf = kmalloc(size);
    if (!buf) buf = kmalloc(size = AUDIO_RING_MIN);
    if (!buf) { busy = 0; return SYS_ENOMEM; }
    ar_init(&ring, buf, size, adpcm);
    wake = adpcm ? AUDIO_WAKE_ADPCM : size / 2u;
    underruns = played = late_max = took_max = 0;
    counting = waiting = 0;
    space = (struct sem){ 0 };
    hardware(1);
    return 0;
}

/* One writer at a time, also among tasks sharing the open file (a program
 * and the workers it spawns with it as their output): the ring has a single
 * producer. Waiting for another writer ends when this task is killed. */
static struct mutex writer;

int audio_write(const uint8_t *p, uint32_t n)
{
    if (mutex_lock_killable(&writer)) return 0;
    uint32_t done = 0;
    while (done < n && !ring.bad) {
        if (ar_used(&ring) > ring.size) panic("audio: ring holds %u of %u", ar_used(&ring), ring.size);
        uint32_t h = ring.head;
        done += ar_put(&ring, p + done, n - done);
        if (ring.head != h) counting = 1;    /* samples to play (a record header alone is none) */
        if (done == n || ring.bad) break;
        if (task_self()->killed) break;
        waiting = 1;                 /* the interrupt gives space once wake bytes are free */
        sem_take(&space, 20);
    }
    int bad = ring.bad;
    ring.bad = 0;
    mutex_unlock(&writer);
    return bad ? SYS_EINVAL : (int)done;
}

void audio_close(void)
{
    counting = 0;
    while (!ar_empty(&ring) && !task_self()->killed) msleep(10);
    hardware(0);
    kprintf("audio: %u samples played, %u short, latency up to %u us, interrupt up to %u cycles, %u B ring\n",
            played, underruns, late_max / (HCLK_HZ / 1000000u), took_max, ring.size);
    kfree(ring.buf);
    ring.buf = 0;
    busy = 0;
}
