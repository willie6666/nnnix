/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Pipes: a 128-byte ring between a write end and a read end, as two files.
 * Reads wait while empty and return 0 once every writer has closed; writes wait
 * while full and fail with EPIPE once every reader has closed. Waits can be
 * interrupted by a kill. */
#include "vfs.h"
#include "../arch/nuc140/arch.h"
#include "../kernel/kmem.h"
#include "../kernel/task.h"

#define PIPE_SIZE 128u
#define EPIPE 32

struct pipe {
    uint8_t buf[PIPE_SIZE];
    uint16_t head, len;
    uint8_t readers, writers;
    struct waitq readable, writable;
};

static void wake_all(struct waitq *q)
{
    struct task *t;
    while ((t = waitq_pop(q))) sched_wake(t, 0);
}

static int writer_end(const struct file *f) { return (f->flags & O_ACCMODE) == O_WRONLY; }

static int p_read(struct file *f, void *buf, uint32_t n)
{
    struct pipe *p = f->priv;
    uint8_t *out = buf;
    if (writer_end(f)) return SYS_EBADF;
    uint32_t s = irq_save();
    while (!p->len) {
        if (!p->writers) { irq_restore(s); return 0; }
        int r = sched_block_interruptible(&p->readable, 0, s);
        if (r) return r;
        s = irq_save();
    }
    uint32_t k = 0;
    for (; k < n && p->len; ++k, --p->len) {
        out[k] = p->buf[p->head];
        p->head = (uint16_t)((p->head + 1u) % PIPE_SIZE);
    }
    wake_all(&p->writable);
    irq_restore(s);
    return (int)k;
}

static int p_write(struct file *f, const void *buf, uint32_t n)
{
    struct pipe *p = f->priv;
    const uint8_t *in = buf;
    uint32_t done = 0;
    if (!writer_end(f)) return SYS_EBADF;
    uint32_t s = irq_save();
    while (done < n) {
        if (!p->readers) {
            struct task *self = task_self();
            if (self->user) self->killed = 1;     /* like SIGPIPE: leaves at its next system call */
            irq_restore(s);
            return done ? (int)done : -EPIPE;
        }
        if (p->len == PIPE_SIZE) {
            int r = sched_block_interruptible(&p->writable, 0, s);
            if (r) return done ? (int)done : r;
            s = irq_save();
            continue;
        }
        for (; done < n && p->len < PIPE_SIZE; ++done, ++p->len)
            p->buf[(p->head + p->len) % PIPE_SIZE] = in[done];
        wake_all(&p->readable);
    }
    irq_restore(s);
    return (int)n;
}

static int p_close(struct file *f)
{
    struct pipe *p = f->priv;
    uint32_t s = irq_save();
    if (writer_end(f)) { p->writers--; wake_all(&p->readable); }
    else { p->readers--; wake_all(&p->writable); }
    int unused = !p->readers && !p->writers;
    irq_restore(s);
    if (unused) kfree(p);
    return 0;
}

static const struct fs_ops pipe_ops = { .close = p_close, .read = p_read, .write = p_write };
static struct mount pipe_mount = { "pipe:", &pipe_ops, 0, 0 };

int vfs_pipe(int fds[2])
{
    struct pipe *p = kmalloc(sizeof *p);
    struct file *r = kmalloc(sizeof *r), *w = kmalloc(sizeof *w);
    if (!p || !r || !w) { kfree(p); kfree(r); kfree(w); return SYS_ENOMEM; }
    *p = (struct pipe){ .readers = 1, .writers = 1 };
    *r = (struct file){ .mnt = &pipe_mount, .priv = p, .flags = O_RDONLY, .refs = 1 };
    *w = (struct file){ .mnt = &pipe_mount, .priv = p, .flags = O_WRONLY, .refs = 1 };
    int e = vfs_install(r);
    if (e < 0) { kfree(p); kfree(r); kfree(w); return e; }
    fds[0] = e;
    e = vfs_install(w);
    if (e < 0) { p->writers = 0; vfs_close(fds[0]); kfree(w); return e; }   /* frees p */
    fds[1] = e;
    return 0;
}
