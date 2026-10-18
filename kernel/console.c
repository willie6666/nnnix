/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "console.h"
#include "sync.h"
#include "task.h"
#include "tty.h"
#include "klog.h"
#include "vt.h"
#include "kmem.h"
#include "scrollback.h"
#include "../arch/nuc140/arch.h"
#include "../drivers/font3x5.h"
#include "../drivers/font5x7.h"
#include "../drivers/kbd.h"
#include "../drivers/lcd.h"
#include "../lib/sys.h"
#include "../user/nsh.h"

#define INPUT_QUEUE 64u

static struct task *shell_task;

static struct vt vt;
static struct tty tty0;
static uint8_t queue[INPUT_QUEUE];
static uint32_t head, tail;          /* protected by irq_save */
static struct sem input_ready;

/* The LCD is shared by the terminal (any task writing to tty0, kd's notices)
 * and /dev/lcd: lcd_lock keeps their SPI transfers apart. */
static struct mutex lcd_lock;
static volatile uint8_t graphics;    /* /dev/lcd owns the picture */
static uint16_t graphics_owner;      /* the pid that opened it */

/* ---- The text on the LCD ----
 * Two sizes: the 5x7 font in 6x8 cells (21x8) and the 3x5 font in 4x6 cells
 * (32x10). A 4x6 row straddles the LCD's 8-pixel pages, so a page is built
 * from the (at most two) rows it shows, column by column, and sent as it is
 * computed: no frame buffer, little stack. Everything below runs under
 * lcd_lock. */
static const struct shape { uint8_t w, h, cols, rows; } shapes[2] = { { 6, 8, 21, 8 }, { 4, 6, 32, 10 } };
static uint8_t small;                /* the size in use: shapes[small] */

/* Scrollback (scrollback.h), in a ring allocated while the console shell
 * runs (hist_size bytes). view: how many lines back the screen shows, 0 for
 * the live terminal. */
static struct scrollback hist;
static uint32_t hist_size = 512;
static uint32_t view;

static void scrolled(void *ctx, const char *row, int n)
{
    (void)ctx;
    scrollback_push(&hist, row, n);
    if (view && view < scrollback_lines(&hist)) view++;   /* the screen keeps showing the same lines */
}

static int unscroll(void *ctx, char *row, int n)
{
    (void)ctx;
    return !view && scrollback_pop(&hist, row, n);
}

/* Row r of the screen as shown: the terminal's, or one from the scrollback
 * (into buf); 0 for a blank one. */
static const char *shown_row(int r, char *buf)
{
    const struct shape *s = &shapes[small];
    if (r < 0 || r >= s->rows) return 0;
    if (!view) return vt.grid[r];
    int from_top = (int)view - r;                    /* the screen's top is `view` lines back */
    if (from_top <= 0) return vt.grid[-from_top];
    return scrollback_line(&hist, (uint32_t)from_top - 1u, buf, VT_COLS_MAX) ? buf : 0;
}

/* Where the pixels go: the LCD, or a frame (console_frame). */
static uint8_t *frame;
static int frame_at;

static void run_begin(int p, int x_hi) { if (frame) frame_at = p * 128 + x_hi; else lcd_run_begin(p, x_hi); }
static void run_byte(uint8_t b) { if (frame) frame[frame_at--] = b; else lcd_run_byte(b); }
static void run_end(void) { if (!frame) lcd_run_end(); }

/* Pixel column i of character c, bit 0 at the top. */
static uint32_t glyph(char c, int i)
{
    unsigned ch = (unsigned char)c >= 0x20 && (unsigned char)c <= 0x7f ? (unsigned char)c - 0x20u : '?' - 0x20u;
    if (!small) return i < 5 ? font5x7[ch][i] : 0;
    return i < 3 ? (font3x5[ch] >> (5 * i)) & 0x1fu : 0;
}

/* The pixel columns x_hi down to x_lo of page p, which shows rows r0 = the
 * first that reaches into it, with text a, and r0 + 1 with text b (0: blank). */
static void paint_rows(int p, int x_lo, int x_hi, const char *a, const char *b)
{
    const struct shape *s = &shapes[small];
    int r0 = 8 * p / s->h, top0 = r0 * s->h - 8 * p, top1 = top0 + s->h;
    int cx = view ? -1 : vt.x, cy = vt.y, under = 1 << (s->h - 1);
    if (top1 >= 8) b = 0;
    run_begin(p, x_hi);
    for (int x = x_hi; x >= x_lo; --x) {
        int col = x / s->w, i = x % s->w;
        uint32_t bits = 0;
        if (col < s->cols) {
            if (a) {
                uint32_t g = glyph(a[col], i) | (col == cx && r0 == cy ? (uint32_t)under : 0);
                bits |= top0 >= 0 ? g << top0 : g >> -top0;
            }
            if (b) bits |= (glyph(b[col], i) | (col == cx && r0 + 1 == cy ? (uint32_t)under : 0)) << top1;
        }
        run_byte((uint8_t)bits);
    }
    run_end();
}

/* Page p of the live terminal: its rows come straight from the grid (this
 * runs on the stack of whoever writes to the console). */
static void paint(int p, int x_lo, int x_hi)
{
    const struct shape *s = &shapes[small];
    int r0 = 8 * p / s->h;
    paint_rows(p, x_lo, x_hi, r0 < s->rows ? vt.grid[r0] : 0, r0 + 1 < s->rows ? vt.grid[r0 + 1] : 0);
}

/* The whole screen as shown, also scrolled back (rows from the scrollback):
 * for the console's keys and console_frame, not on writers' stacks. */
static void paint_all(void)
{
    const struct shape *s = &shapes[small];
    char buf[2][VT_COLS_MAX];
    for (int p = 0; p < 8; ++p) {
        int r0 = 8 * p / s->h;
        paint_rows(p, 0, 127, shown_row(r0, buf[0]), shown_row(r0 + 1, buf[1]));
    }
}

/* Pixel columns waiting to be drawn: [dirty_lo, dirty_hi] of each page, none
 * when lo > hi. The terminal's callbacks only mark them, and flush() draws
 * once vt_write has returned: drawing deep under the terminal's own calls
 * (a newline's repaint) took a program printing to the LCD 176 B deeper into
 * its stack than one printing to telnet. A page is also drawn once however
 * many of its cells changed. */
static uint8_t dirty_lo[8] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff }, dirty_hi[8];

static void mark(int p, int lo, int hi)
{
    if (lo < dirty_lo[p]) dirty_lo[p] = (uint8_t)lo;
    if (hi > dirty_hi[p]) dirty_hi[p] = (uint8_t)hi;
}

static void flush(void)
{
    for (int p = 0; p < 8; ++p) {
        if (dirty_lo[p] <= dirty_hi[p] && !graphics && !view) paint(p, dirty_lo[p], dirty_hi[p]);
        dirty_lo[p] = 0xff;
        dirty_hi[p] = 0;
    }
}

/* The terminal's own repaints; scrolled back, the screen stays. */
static void redraw(void *ctx)
{
    (void)ctx;
    for (int p = 0; p < 8; ++p) mark(p, 0, 127);
}

static void draw(void *ctx, int col, int row, char c, int cursor)
{
    (void)ctx; (void)c; (void)cursor;
    const struct shape *s = &shapes[small];
    for (int p = row * s->h / 8; p <= (row * s->h + s->h - 1) / 8; ++p)
        mark(p, col * s->w, col * s->w + s->w - 1);
}

/* The console's own keys (kbd.h); others while scrolled back return to the
 * terminal first. */
static void key(uint8_t k)
{
    const struct shape *s = &shapes[small];
    uint32_t was = view, max = scrollback_lines(&hist);
    if (k == KBD_PAGE_UP) view = view + s->rows - 1u < max ? view + s->rows - 1u : max;
    else if (k == KBD_LINE_UP) view = view < max ? view + 1u : max;
    else if (k == KBD_PAGE_DOWN) view = view > s->rows - 1u ? view - (s->rows - 1u) : 0;
    else if (k == KBD_LINE_DOWN) view = view ? view - 1u : 0;
    else if (k == KBD_TEXT_SMALL || k == KBD_TEXT_LARGE) {
        small = k == KBD_TEXT_SMALL;
        view = 0;
        vt_resize(&vt, shapes[small].cols, shapes[small].rows);
        flush();
        return;
    } else view = 0;
    if (view != was && !graphics) paint_all();
}

static void text(const char *s, size_t n)
{
    mutex_lock(&lcd_lock);
    vt_write(&vt, s, n);
    flush();
    mutex_unlock(&lcd_lock);
}

int console_lcd_claim(void)
{
    uint32_t s = irq_save();
    int busy = graphics;
    graphics = 1;
    if (!busy) graphics_owner = task_self()->pid;
    irq_restore(s);
    return busy ? SYS_EBUSY : 0;
}

uint16_t console_lcd_owner(void) { return graphics ? graphics_owner : 0; }

void console_lcd_write(uint32_t off, const void *p, uint32_t n)
{
    mutex_lock(&lcd_lock);
    lcd_bytes(off, p, n);
    mutex_unlock(&lcd_lock);
}

void console_lcd_release(void)
{
    mutex_lock(&lcd_lock);
    graphics = 0;
    graphics_owner = 0;
    view = 0;
    lcd_clear();
    vt_redraw(&vt);
    flush();
    mutex_unlock(&lcd_lock);
}

void console_input(const uint8_t *bytes, int n)
{
    int queued = 0;
    for (int i = 0; i < n; ++i) {
        if (bytes[i] >= 0x80u || view) {             /* the console's keys; any key ends scrolling */
            mutex_lock(&lcd_lock);
            key(bytes[i]);
            mutex_unlock(&lcd_lock);
            if (bytes[i] >= 0x80u) continue;
        }
        uint32_t s = irq_save();
        if (head - tail < INPUT_QUEUE) { queue[head++ % INPUT_QUEUE] = bytes[i]; queued = 1; }
        irq_restore(s);
    }
    if (queued) sem_give(&input_ready);
}

/* The scrollback ring, while the console shell runs: a new one, empty, or
 * none. 0, or -1 when there is no memory for the new one (the old one stays). */
static int history(int on)
{
    char *buf = on && hist_size ? kmalloc_high(hist_size) : 0;
    if (on && hist_size && !buf) return -1;
    mutex_lock(&lcd_lock);
    kfree(hist.buf);
    scrollback_init(&hist, buf, hist_size);
    if (view) { view = 0; redraw(0); flush(); }
    mutex_unlock(&lcd_lock);
    return 0;
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
    text(s, n);
}

static const struct tty_ops console_ops = { console_getc, console_write, console_poll_byte };

/* The screen's text as shown (scrolled back or not), for tests. */
size_t console_snapshot(char *buf, size_t size)
{
    char row[VT_COLS_MAX];
    size_t n = 0;
    mutex_lock(&lcd_lock);
    for (int r = 0; r < shapes[small].rows; ++r) {
        const char *t = shown_row(r, row);
        int end = t ? shapes[small].cols : 0;
        while (end > 0 && t[end - 1] == ' ') --end;
        for (int c = 0; c < end && n < size; ++c) buf[n++] = t[c];
        if (n < size) buf[n++] = '\n';
    }
    mutex_unlock(&lcd_lock);
    return n;
}

void console_frame(uint8_t out[1024])
{
    mutex_lock(&lcd_lock);
    frame = out;
    paint_all();
    frame = 0;
    mutex_unlock(&lcd_lock);
}

int console_text_size(int s)
{
    if (s > 1) return -1;
    if (s >= 0 && s != small) {
        mutex_lock(&lcd_lock);
        key(s ? KBD_TEXT_SMALL : KBD_TEXT_LARGE);
        mutex_unlock(&lcd_lock);
    }
    return small;
}

int console_history(int bytes)
{
    if (bytes > 8192) return SYS_EINVAL;
    if (bytes >= 0 && (uint32_t)bytes != hist_size) {
        uint32_t was = hist_size;
        hist_size = (uint32_t)bytes;
        if (shell_task && history(1)) {              /* a new ring, empty */
            hist_size = was;
            return SYS_ENOMEM;
        }
    }
    return shell_task && !hist.buf ? 0 : (int)hist_size;
}


static void shell(void *arg)
{
    (void)arg;
    task_self()->tty = &tty0;
    nsh_main();
}

static void start_shell(void)
{
    static const char clear[] = "\x1b[2J\x1b[H";
    text(clear, sizeof clear - 1);
    shell_task = task_spawn("nsh", shell, 0, NSH_STACK, 5, TASK_LONG_LIVED);
    if (shell_task && history(1)) kprintf("console: no memory for the %u-byte scrollback\n", hist_size);
    notice_shown = 0;
}

/* Without a shell, "ask" waits for Enter and "off" ignores the keyboard; either
 * way the shell's 1.5 KiB (stack and state) stay free. */
void console_poll(void)
{
    if (shell_task && shell_task->state == TASK_ZOMBIE) {
        task_join(shell_task, 0);
        shell_task = 0;
        history(0);
    }
    if (shell_task) return;
    if (mode == CONSOLE_ON) { start_shell(); return; }
    if (!notice_shown) {
        static const char ask[] = "\x1b[2J\x1b[Hnnnix console\r\n\r\nPress Enter to\r\nlog in.";
        static const char off[] = "\x1b[2J\x1b[Hnnnix console\r\n\r\nThe console is off.";
        if (mode == CONSOLE_ASK) text(ask, sizeof ask - 1);
        else text(off, sizeof off - 1);
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
    vt.redraw = redraw;
    vt.scrolled = scrolled;
    vt.unscroll = unscroll;
    flush();
    tty_init(&tty0, &console_ops, 0);
}
