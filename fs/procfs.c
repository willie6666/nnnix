/* SPDX-License-Identifier: GPL-3.0-or-later */
/* "/proc": read-only text generated when the file is opened. */
#include <stdarg.h>
#include "vfs.h"
#include "../kernel/kernel.h"
#include "../kernel/klog.h"
#include "../kernel/kmem.h"
#include "../kernel/modules.h"
#include "../kernel/task.h"
#include "../lib/fmt.h"
#include "../lib/string.h"
#include "../arch/nuc140/arch.h"

#define TEXT_MAX 512u

struct text { uint32_t len; char data[]; };
struct gen { struct text *t; };

static void put(void *ctx, char c)
{
    struct gen *g = ctx;
    if (g->t->len < TEXT_MAX) g->t->data[g->t->len++] = c;
}

static void say(struct gen *g, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void say(struct gen *g, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    nn_vformat(put, g, fmt, ap);
    va_end(ap);
}

static void gen_version(struct gen *g) { say(g, "nnnix %s NUC140VE3CN\n", nnnix_version); }
static void gen_uptime(struct gen *g) { say(g, "%u.%03u\n", jiffies / 1000u, jiffies % 1000u); }

static void gen_meminfo(struct gen *g)
{
    struct kmem_stats m;
    kmem_stats(&m);
    say(g, "heap_total %u\nheap_free %u\nheap_largest %u\nstatic %u\nmsp_used %u\n",
        m.total, m.free, m.largest, ram_static(), stack_used());
}

static void gen_tasks(struct gen *g)
{
    struct task_info info[4];
    say(g, "pid name prio state stack_used stack_size cpu_ms\n");
    for (int skip = 0, n; (n = task_list(info, 4, skip)) > 0; skip += n)
        for (int i = 0; i < n; ++i)
            say(g, "%u %s %u %u %u %u %u\n", info[i].pid, info[i].name, info[i].prio, info[i].state,
                info[i].stack_used, info[i].stack_size, info[i].cpu_ms);
}

static void gen_modules(struct gen *g)
{
    say(g, "power %u\nch376 %u\nw5500 %u\nlink 0x%x\n", modules.powered, modules.ch376, modules.w5500, modules.link);
}

static void gen_klog(struct gen *g) { g->t->len = (uint32_t)klog_read(g->t->data, TEXT_MAX); }
static void gen_crash(struct gen *g) { if (fault_last_report()[0]) say(g, "%s\n", fault_last_report()); }

static const struct { const char *name; void (*gen)(struct gen *g); } files[] = {
    { "version", gen_version }, { "uptime", gen_uptime }, { "meminfo", gen_meminfo },
    { "tasks", gen_tasks }, { "modules", gen_modules }, { "klog", gen_klog }, { "crash", gen_crash },
};
#define FILES (sizeof files / sizeof files[0])

static int find(const char *path)
{
    if (!strcmp(path, "/")) return -1;
    for (unsigned i = 0; i < FILES; ++i) if (!strcmp(path + 1, files[i].name)) return (int)i;
    return -2;
}

static int p_open(struct mount *m, const char *path, int flags, struct file *f)
{
    (void)m;
    int i = find(path);
    if (i == -2) return flags & O_CREAT ? SYS_EROFS : SYS_ENOENT;
    if ((flags & O_ACCMODE) != O_RDONLY) return i == -1 ? SYS_EISDIR : SYS_EROFS;
    if (i == -1) { f->priv = 0; return 0; }         /* the directory */
    struct gen g = { kmalloc(sizeof(struct text) + TEXT_MAX) };
    if (!g.t) return SYS_ENOMEM;
    g.t->len = 0;
    files[i].gen(&g);
    f->priv = g.t;
    return 0;
}

static int p_close(struct file *f) { kfree(f->priv); return 0; }

static int p_read(struct file *f, void *buf, uint32_t n)
{
    struct text *t = f->priv;
    if (!t) return SYS_EISDIR;
    if (f->pos >= t->len) return 0;
    if (n > t->len - f->pos) n = t->len - f->pos;
    memcpy(buf, t->data + f->pos, n);
    f->pos += n;
    return (int)n;
}

static int p_readdir(struct file *f, struct nn_dirent *d)
{
    if (f->priv) return SYS_ENOTDIR;
    if (f->pos >= FILES) return 0;
    strcpy(d->name, files[f->pos++].name);
    d->size = 0;
    d->type = NN_TYPE_FILE;
    return 1;
}

static int p_stat(struct mount *m, const char *path, struct nn_stat *st)
{
    (void)m;
    int i = find(path);
    if (i == -2) return SYS_ENOENT;
    st->size = 0;
    st->type = i == -1 ? NN_TYPE_DIR : NN_TYPE_FILE;
    return 0;
}

const struct fs_ops procfs_ops = { .open = p_open, .close = p_close, .read = p_read,
                                   .readdir = p_readdir, .stat = p_stat };
