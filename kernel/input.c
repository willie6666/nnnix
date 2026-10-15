/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "input.h"
#include "console.h"
#include "klog.h"
#include "../arch/nuc140/arch.h"
#include "../drivers/uart.h"
#include "../drivers/ch9350.h"
#include "../drivers/kbd.h"

static struct ch9350_parser parser;
static struct kbd kbd;
static uint32_t mouse_reports;

uint32_t input_mouse_reports(void) { return mouse_reports; }
uint32_t input_bad_bytes(void) { return parser.bad_bytes; }

static void event(const struct ch9350_event *ev)
{
    uint8_t bytes[24];
    switch (ev->type) {
    case CH9350_KEYBOARD:
        console_input(bytes, kbd_report(&kbd, ev->data, jiffies, bytes, sizeof bytes));
        break;
    case CH9350_MOUSE:
        mouse_reports++;             /* /dev/mouse arrives with the VFS (M4) */
        break;
    case CH9350_REMOVED:
        kprintf("ch9350: device removed\n");
        kbd = (struct kbd){ 0 };
        break;
    default:
        break;
    }
}

void input_poll(void)
{
    int c;
    struct ch9350_event ev;
    uint8_t bytes[8];
    while ((c = uart1_getc()) >= 0)
        if (ch9350_feed(&parser, (uint8_t)c, &ev)) event(&ev);
    console_input(bytes, kbd_repeat(&kbd, jiffies, bytes, sizeof bytes));
}

uint32_t input_next_ms(void)
{
    if (!kbd.held) return UINT32_MAX;
    int32_t left = (int32_t)(kbd.repeat_at - jiffies);
    return left > 0 ? (uint32_t)left : 0;
}
