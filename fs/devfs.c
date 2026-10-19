/* SPDX-License-Identifier: GPL-3.0-or-later */
/* "/dev": null, zero, tty (the caller's terminal), led (write 0 or 1), lcd,
 * audio, input, adpcm.
 *
 * lcd: the onboard display as a 1024-byte frame (layout in drivers/lcd.h).
 * Opening it for writing takes the display from the console text until the
 * file is closed (also when the program ends or is killed); one writer at a
 * time, others get EBUSY. Writes go to the position modulo 1024, so writing
 * whole frames one after another needs no seeking.
 *
 * audio: the buzzer (drivers/audio.h): unsigned 8-bit mono samples at 8 kHz.
 * One writer at a time; writes block while the buffer (256 ms, 128 ms when
 * memory is short) is full; the close plays out what is queued.
 * adpcm: the same buzzer, taking NNV sound records (lib/nnv.h), twice as long
 * in the buffer; one writer between the two.
 *
 * input: keyboard, mouse and board keypad events (kernel/input.h, struct
 * nn_input_event in lib/sys.h) for one reader at a time, others get EBUSY;
 * while it is open the keyboard does not type into the console. */
#include "vfs.h"
#include "../arch/nuc140/arch.h"
#include "../kernel/console.h"
#include "../kernel/input.h"
#include "../kernel/task.h"
#include "../kernel/tty.h"
#include "../drivers/audio.h"
#include "../drivers/lcd.h"
#include "../lib/string.h"

enum { DEV_DIR, DEV_NULL, DEV_ZERO, DEV_TTY, DEV_LED, DEV_LCD, DEV_AUDIO, DEV_INPUT, DEV_ADPCM };
static const char *const names[] = { 0, "null", "zero", "tty", "led", "lcd", "audio", "input", "adpcm" };
#define DEVICES (sizeof names / sizeof names[0])

static int find(const char *path)
{
    if (!strcmp(path, "/")) return DEV_DIR;
    for (unsigned i = 1; i < DEVICES; ++i) if (!strcmp(path + 1, names[i])) return (int)i;
    return SYS_ENOENT;
}

static int d_open(struct mount *m, const char *path, int flags, struct file *f)
{
    (void)m;
    int dev = find(path);
    if (dev < 0) return flags & O_CREAT ? SYS_EROFS : dev;
    if (dev == DEV_DIR && (flags & O_ACCMODE) != O_RDONLY) return SYS_EISDIR;
    if (dev == DEV_TTY && !task_self()->tty) return SYS_ENODEV;
    if ((dev == DEV_LCD || dev == DEV_AUDIO || dev == DEV_ADPCM) && (flags & O_ACCMODE) != O_RDONLY) {
        int e = dev == DEV_LCD ? console_lcd_claim() : audio_open(dev == DEV_ADPCM);
        if (e) return e;
    }
    if (dev == DEV_INPUT) {
        if ((flags & O_ACCMODE) != O_RDONLY) return SYS_EINVAL;
        int e = input_claim();
        if (e) return e;
    }
    f->priv = (void *)(uintptr_t)dev;
    return 0;
}

static int d_read(struct file *f, void *buf, uint32_t n)
{
    switch ((uintptr_t)f->priv) {
    case DEV_ZERO: memset(buf, 0, n); return (int)n;
    case DEV_TTY: return tty_read(task_self()->tty, buf, n);
    case DEV_INPUT: return input_read(buf, n, f->flags & O_NONBLOCK);
    case DEV_DIR: return SYS_EISDIR;
    default: return 0;
    }
}

static int d_write(struct file *f, const void *buf, uint32_t n)
{
    switch ((uintptr_t)f->priv) {
    case DEV_TTY: tty_write(task_self()->tty, buf, n); return (int)n;
    case DEV_LED: if (n) board_led_set(((const char *)buf)[0] == '1'); return (int)n;
    case DEV_LCD: {
        if ((f->flags & O_ACCMODE) == O_RDONLY) return SYS_EBADF;
        const uint8_t *p = buf;
        for (uint32_t left = n; left;) {
            uint32_t off = f->pos % LCD_FRAME, run = LCD_FRAME - off;
            if (run > left) run = left;
            console_lcd_write(off, p, run);
            f->pos += run;
            p += run;
            left -= run;
        }
        return (int)n;
    }
    case DEV_AUDIO:
    case DEV_ADPCM:
        if ((f->flags & O_ACCMODE) == O_RDONLY) return SYS_EBADF;
        return audio_write(buf, n);
    case DEV_DIR: return SYS_EISDIR;
    default: return (int)n;
    }
}

static int d_close(struct file *f)
{
    if ((uintptr_t)f->priv == DEV_INPUT) input_release();
    if ((f->flags & O_ACCMODE) == O_RDONLY) return 0;
    if ((uintptr_t)f->priv == DEV_LCD) console_lcd_release();
    if ((uintptr_t)f->priv == DEV_AUDIO || (uintptr_t)f->priv == DEV_ADPCM) audio_close();
    return 0;
}

static int d_lseek(struct file *f, int32_t off, int whence)
{
    if ((uintptr_t)f->priv != DEV_LCD) return SYS_EINVAL;
    int32_t pos = vfs_seek_pos(f->pos, LCD_FRAME, off, whence);
    if (pos < 0) return pos;
    f->pos = (uint32_t)pos;
    return pos;
}

static int d_readdir(struct file *f, struct nn_dirent *d)
{
    if ((uintptr_t)f->priv != DEV_DIR) return SYS_ENOTDIR;
    if (f->pos + 1 >= DEVICES) return 0;
    strcpy(d->name, names[++f->pos]);
    d->size = 0;
    d->type = NN_TYPE_DEV;
    return 1;
}

static int d_stat(struct mount *m, const char *path, struct nn_stat *st)
{
    (void)m;
    int dev = find(path);
    if (dev < 0) return dev;
    st->size = dev == DEV_LCD ? LCD_FRAME : 0;
    st->type = dev == DEV_DIR ? NN_TYPE_DIR : NN_TYPE_DEV;
    return 0;
}

const struct fs_ops devfs_ops = { .open = d_open, .close = d_close, .read = d_read, .write = d_write,
                                  .lseek = d_lseek, .readdir = d_readdir, .stat = d_stat };
