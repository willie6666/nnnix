/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Keyboard, mouse and the board's keys (input.h). Runs inside the kernel
 * daemon, except input_read and the claim, which run in the program.
 *
 * The board (Nu-LB-NUC140): SW_INT on PB15, pressed low; the 1-9 keypad on
 * PA0-PA5, quasi-bidirectional: PA2, PA1, PA0 low in turn select the columns
 * of keys 1 4 7, 2 5 8 and 3 6 9, and a pressed key pulls PA3, PA4 or PA5 low
 * (its row), as in the board's BSP (Scankey.c). They are scanned every 20 ms,
 * and only while a program holds /dev/input or /dev/lcd: a state counts once
 * two scans agree. */
#include "input.h"
#include "console.h"
#include "inputev.h"
#include "klog.h"
#include "kd.h"
#include "kmem.h"
#include "task.h"
#include "../arch/nuc140/arch.h"
#include "../arch/nuc140/regs.h"
#include "../drivers/uart.h"
#include "../drivers/ch9350.h"
#include "../drivers/kbd.h"

#define SCAN_MS 20u
#define SW_INT  (1u << 9)            /* in a board key mask, beside the keypad's 9 */
#define FORCE_SCANS 150u             /* how long a stop is pushed through: 3 s */

static struct ch9350_parser parser;
static struct kbd kbd;
static uint32_t mouse_reports;

/* The claim (/dev/input): the queue exists while a program holds it. */
struct claim {
    struct input_queue q;
    struct waitq readers;
    uint8_t buttons;                 /* the mouse's, as its last report had them */
};
static struct claim *claim;
static uint16_t owner;               /* its pid */

static uint16_t keys, seen;          /* board keys: debounced, and the last scan */
static uint16_t injected;            /* board keys pressed by diag (tests) */
static uint32_t scan_at;
static uint16_t stopping;            /* pid SW_INT is ending, for stop_scans more scans */
static uint8_t stop_scans;

uint32_t input_mouse_reports(void) { return mouse_reports; }
uint32_t input_bad_bytes(void) { return parser.bad_bytes; }

static uint16_t now_ms(void) { return (uint16_t)jiffies; }

/* Interrupts off. */
static void queue(const struct nn_input_event *e)
{
    if (!claim) return;
    inputq_put(&claim->q, e);
    struct task *t;
    while ((t = waitq_pop(&claim->readers))) sched_wake(t, 0);
}

/* Interrupts off. After dropped events, the reader first gets what changed
 * meanwhile (inputq_sync), from the state before the events at hand. */
static void sync(void)
{
    if (!claim || !claim->q.resync) return;
    struct input_state now = { kbd.prev, kbd.caps, claim->buttons, (uint16_t)(keys & 0x1ffu) };
    inputq_sync(&claim->q, &now, now_ms());
}

static void deliver(const struct nn_input_event *e, int n)
{
    uint32_t s = irq_save();
    sync();
    for (int i = 0; i < n; ++i) queue(&e[i]);
    irq_restore(s);
}

static void event(const struct ch9350_event *ev)
{
    switch (ev->type) {
    case CH9350_KEYBOARD: {
        uint8_t bytes[24];
        struct nn_input_event e[INPUT_KEY_EVENTS];
        if (!input_report_valid(ev->data)) break;      /* rollover: not the keys' state (R52) */
        if (claim) deliver(e, input_key_events(kbd.prev, ev->data, kbd.caps, now_ms(), e, INPUT_KEY_EVENTS));
        int n = kbd_report(&kbd, ev->data, jiffies, bytes, sizeof bytes);   /* the last report, Caps Lock, repeats */
        if (!claim) console_input(bytes, n);
        break;
    }
    case CH9350_MOUSE: {
        mouse_reports++;
        struct nn_input_event e = { NN_INPUT_MOUSE, ev->data[0], ev->data[3], 0, (int8_t)ev->data[1], (int8_t)ev->data[2], now_ms() };
        if (claim) { deliver(&e, 1); claim->buttons = ev->data[0]; }
        break;
    }
    case CH9350_REMOVED: {
        /* Whatever was held is let go: keys, and mouse buttons (R66). */
        kprintf("ch9350: device removed\n");
        struct nn_input_event e[INPUT_KEY_EVENTS + 1];
        static const uint8_t none[8];
        int n = input_key_events(kbd.prev, none, 0, now_ms(), e, INPUT_KEY_EVENTS);
        if (claim && claim->buttons) e[n++] = (struct nn_input_event){ NN_INPUT_MOUSE, 0, 0, 0, 0, 0, now_ms() };
        if (claim) { deliver(e, n); claim->buttons = 0; }
        kbd = (struct kbd){ 0 };
        break;
    }
    default:
        break;
    }
}

/* ---- the board's keys ---- */

void input_init(void)
{
    gpio_mode(PORTB, 15, 3);
    for (unsigned pin = 0; pin < 6; ++pin) gpio_mode(PORTA, pin, 3);
    GPIO_DOUT(PORTA) |= 0x3fu;
}

static uint16_t scan(void)
{
    static const uint8_t first[3] = { 1, 2, 3 };    /* the key of row PA3 with PA2, PA1, PA0 low */
    uint16_t m = (GPIO_PIN(PORTB) & BIT(15)) ? 0 : SW_INT;
    for (int col = 0; col < 3; ++col) {
        GPIO_DOUT(PORTA) = (GPIO_DOUT(PORTA) | 0x3fu) & ~BIT(2 - col);
        for (volatile int i = 0; i < 20; ++i) { }   /* the weak pull-up brings a released row back */
        uint32_t rows = ~GPIO_PIN(PORTA) >> 3;
        for (int row = 0; row < 3; ++row)
            if (rows & (1u << row)) m |= (uint16_t)(1u << (first[col] + 3 * row - 1));
    }
    GPIO_DOUT(PORTA) |= 0x3fu;
    return m | injected;
}

static int scanning(void) { return claim || console_lcd_owner() || stopping; }

/* SW_INT: end the program holding the keys, or the display. The kill takes
 * effect at its next system call; one that runs its own code without making
 * any is ended where it was preempted (task_end_now), tried until it is gone. */
static void stop_holder(void)
{
    uint16_t pid = owner ? owner : console_lcd_owner();
    if (!pid) return;
    int e = task_kill(pid);
    kprintf("input: SW_INT: %s pid %u (%s)\n", e ? "cannot end" : "ending", pid, owner == pid ? "/dev/input" : "/dev/lcd");
    if (!e) { stopping = pid; stop_scans = FORCE_SCANS; }
}

static void board_keys(void)
{
    if ((int32_t)(jiffies - scan_at) < 0) return;
    scan_at = jiffies + SCAN_MS;
    if (stopping && (!task_end_now(stopping) || !--stop_scans)) stopping = 0;
    uint16_t m = scan();
    if (m != seen) { seen = m; return; }          /* not settled yet */
    uint16_t was = keys;
    struct nn_input_event e[9];
    if (claim) deliver(e, input_pad_events(was & 0x1ffu, m & 0x1ffu, now_ms(), e, 9));   /* keys: still the state before */
    keys = m;
    if ((m & SW_INT) && !(was & SW_INT)) stop_holder();
}

void input_inject_keys(uint16_t mask) { injected = mask & 0x3ffu; }

uint32_t input_feed(const uint8_t *p, uint32_t n) { return uart1_inject(p, n); }

void input_poll(void)
{
    int c;
    struct ch9350_event ev;
    uint8_t bytes[8];
    while ((c = uart1_getc()) >= 0)
        if (ch9350_feed(&parser, (uint8_t)c, &ev)) event(&ev);
    int n = kbd_repeat(&kbd, jiffies, bytes, sizeof bytes);
    if (!claim) console_input(bytes, n);
    if (scanning()) board_keys();
    else keys = seen = 0;
}

uint32_t input_next_ms(void)
{
    uint32_t next = UINT32_MAX;
    if (kbd.held) {
        int32_t left = (int32_t)(kbd.repeat_at - jiffies);
        next = left > 0 ? (uint32_t)left : 0;
    }
    if (scanning()) {
        int32_t left = (int32_t)(scan_at - jiffies);
        uint32_t scan_left = left > 0 ? (uint32_t)left : 0;
        if (scan_left < next) next = scan_left;
    }
    return next;
}

/* ---- /dev/input ---- */

int input_claim(void)
{
    struct claim *c = kmalloc(sizeof *c);
    if (!c) return SYS_ENOMEM;
    *c = (struct claim){ 0 };
    uint32_t s = irq_save();
    int busy = claim != 0;
    if (!busy) {
        claim = c;
        owner = task_self()->pid;
        kbd.held = 0;                                  /* no repeats of a key typed before */
        scan_at = jiffies;
    }
    irq_restore(s);
    if (busy) { kfree(c); return SYS_EBUSY; }
    return 0;
}

void input_release(void)
{
    uint32_t s = irq_save();
    struct claim *c = claim;
    claim = 0;
    owner = 0;
    struct task *t;
    while (c && (t = waitq_pop(&c->readers))) sched_wake(t, 0);
    irq_restore(s);
    kfree(c);
}

uint16_t input_owner(void) { return owner; }

int input_read(void *buf, uint32_t n, int nonblock)
{
    if (n < sizeof(struct nn_input_event)) return SYS_EINVAL;
    struct nn_input_event *out = buf;
    uint32_t max = n / sizeof *out, got = 0;
    uint32_t s = irq_save();
    for (;;) {
        if (!claim) { irq_restore(s); return 0; }      /* released under the reader */
        while (got < max && inputq_get(&claim->q, &out[got])) ++got;
        if (got) { sync(); break; }                    /* room again: catch up after a drop */
        if (nonblock) { irq_restore(s); return SYS_EAGAIN; }
        int r = sched_block_interruptible(&claim->readers, 0, s);
        if (r) return r;
        s = irq_save();
    }
    irq_restore(s);
    return (int)(got * sizeof *out);
}
