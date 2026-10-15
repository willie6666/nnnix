/* SPDX-License-Identifier: GPL-3.0-or-later */
/* "/dev": null, zero, tty (the caller's terminal), led (write 0 or 1). */
#include "vfs.h"
#include "../arch/nuc140/arch.h"
#include "../kernel/task.h"
#include "../kernel/tty.h"
#include "../lib/string.h"

enum { DEV_DIR, DEV_NULL, DEV_ZERO, DEV_TTY, DEV_LED };
static const char *const names[] = { 0, "null", "zero", "tty", "led" };
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
    f->priv = (void *)(uintptr_t)dev;
    return 0;
}

static int d_read(struct file *f, void *buf, uint32_t n)
{
    switch ((uintptr_t)f->priv) {
    case DEV_ZERO: memset(buf, 0, n); return (int)n;
    case DEV_TTY: return tty_read(task_self()->tty, buf, n);
    case DEV_DIR: return SYS_EISDIR;
    default: return 0;
    }
}

static int d_write(struct file *f, const void *buf, uint32_t n)
{
    switch ((uintptr_t)f->priv) {
    case DEV_TTY: tty_write(task_self()->tty, buf, n); return (int)n;
    case DEV_LED: if (n) board_led_set(((const char *)buf)[0] == '1'); return (int)n;
    case DEV_DIR: return SYS_EISDIR;
    default: return (int)n;
    }
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
    st->size = 0;
    st->type = dev == DEV_DIR ? NN_TYPE_DIR : NN_TYPE_DEV;
    return 0;
}

const struct fs_ops devfs_ops = { .open = d_open, .read = d_read, .write = d_write,
                                  .readdir = d_readdir, .stat = d_stat };
