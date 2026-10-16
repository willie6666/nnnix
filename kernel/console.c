/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "console.h"
#include "sync.h"
#include "task.h"
#include "tty.h"
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

static int console_getc(void *ctx)
{
    (void)ctx;
    for (;;) {
        uint32_t s = irq_save();
        if (tail != head) {
            int c = queue[tail++ % INPUT_QUEUE];
            irq_restore(s);
            return c;
        }
        irq_restore(s);
        sem_take(&input_ready, SEM_FOREVER);
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

void console_poll(void)
{
    if (shell_task && shell_task->state == TASK_ZOMBIE) {
        task_join(shell_task, 0);
        shell_task = 0;
    }
    if (!shell_task) shell_task = task_spawn("nsh", shell, 0, 1280, 5, 0);
}

void console_init(void)
{
    lcd_init();
    vt_init(&vt, draw, 0);
    tty_init(&tty0, &console_ops, 0);
}
