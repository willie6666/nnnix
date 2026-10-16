/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "console.h"
#include "sync.h"
#include "task.h"
#include "tty.h"
#include "klog.h"
#include "vt.h"
#include "../arch/nuc140/arch.h"
#include "../drivers/lcd.h"
#include "../user/nsh.h"

#define INPUT_QUEUE 64u

static struct vt vt;
static struct tty tty0;
static uint8_t queue[INPUT_QUEUE];
static uint32_t head, tail;          /* protected by irq_save */
static struct sem input_ready;

static void draw(void *ctx, int col, int row, char c, int cursor)
{
    (void)ctx;
    lcd_char(col, row, c, cursor);
}

void console_input(const uint8_t *bytes, int n)
{
    uint32_t s = irq_save();
    for (int i = 0; i < n; ++i)
        if (head - tail < INPUT_QUEUE) queue[head++ % INPUT_QUEUE] = bytes[i];
    irq_restore(s);
    if (n) sem_give(&input_ready);
}

static volatile uint8_t mode = CONSOLE_ASK;
static uint8_t notice_shown;         /* the ask/off notice is on the screen */

static int console_getc(void *ctx, uint32_t timeout_ms)
{
    (void)ctx;
    uint32_t start = jiffies;
    for (;;) {
        uint32_t s = irq_save();
        if (tail != head) {
            int c = queue[tail++ % INPUT_QUEUE];
            irq_restore(s);
            return c;
        }
        irq_restore(s);
        /* A killed program sees end of input; so does the shell when the console is turned off. */
        if (task_self()->killed || mode == CONSOLE_OFF) return -1;
        uint32_t wait = 200;
        if (timeout_ms) {
            uint32_t waited = jiffies - start;
            if (waited >= timeout_ms) return TTY_TIMEOUT;
            if (timeout_ms - waited < wait) wait = timeout_ms - waited;
        }
        sem_take(&input_ready, wait);
    }
}

static int console_poll_byte(void *ctx)
{
    (void)ctx;
    uint32_t s = irq_save();
    int c = tail != head ? queue[tail++ % INPUT_QUEUE] : -1;
    irq_restore(s);
    return c;
}

static void console_write(void *ctx, const char *s, size_t n)
{
    (void)ctx;
    vt_write(&vt, s, n);
}

static const struct tty_ops console_ops = { console_getc, console_write, console_poll_byte };

size_t console_snapshot(char *buf, size_t size) { return vt_snapshot(&vt, buf, size); }

static struct task *shell_task;

static void shell(void *arg)
{
    (void)arg;
    task_self()->tty = &tty0;
    nsh_main();
}

static void start_shell(void)
{
    static const char clear[] = "\x1b[2J\x1b[H";
    vt_write(&vt, clear, sizeof clear - 1);
    shell_task = task_spawn("nsh", shell, 0, 1280, 5, TASK_LONG_LIVED);
    notice_shown = 0;
}

/* Without a shell, "ask" waits for Enter and "off" ignores the keyboard; either
 * way the shell's 1.5 KiB (stack and state) stay free. */
void console_poll(void)
{
    if (shell_task && shell_task->state == TASK_ZOMBIE) {
        task_join(shell_task, 0);
        shell_task = 0;
    }
    if (shell_task) return;
    if (mode == CONSOLE_ON) { start_shell(); return; }
    if (!notice_shown) {
        static const char ask[] = "\x1b[2J\x1b[Hnnnix console\r\n\r\nPress Enter to\r\nlog in.";
        static const char off[] = "\x1b[2J\x1b[Hnnnix console\r\n\r\nThe console is off.";
        if (mode == CONSOLE_ASK) vt_write(&vt, ask, sizeof ask - 1);
        else vt_write(&vt, off, sizeof off - 1);
        notice_shown = 1;
    }
    int enter = 0;
    uint32_t s = irq_save();
    while (tail != head) {
        uint8_t c = queue[tail++ % INPUT_QUEUE];
        if (c == '\r' || c == '\n') enter = 1;
    }
    irq_restore(s);
    if (enter && mode == CONSOLE_ASK) start_shell();
}

int console_mode(int m)
{
    static const char *const names[] = { "off", "ask", "on" };
    if (m > CONSOLE_ON) return -1;
    if (m >= 0 && m != mode) {
        mode = (uint8_t)m;
        notice_shown = 0;                 /* redraw the notice for the new mode */
        kprintf("console: %s\n", names[m]);
    }
    return mode;
}

void console_init(void)
{
    lcd_init();
    vt_init(&vt, draw, 0);
    tty_init(&tty0, &console_ops, 0);
}
