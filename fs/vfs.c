/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "vfs.h"
#include "../kernel/klog.h"
#include "../kernel/kmem.h"
#include "../kernel/panic.h"
#include "../kernel/sync.h"
#include "../kernel/task.h"
#include "../lib/string.h"
#include "../arch/nuc140/arch.h"

#define FSD_STACK 1536u

static struct mount mounts[] = {
    { "/usb", &usbfs_ops, 0, 1 },
    { "/dev", &devfs_ops, 0, 0 },
    { "/proc", &procfs_ops, 0, 0 },
    { "/", &lfs_ops, 0, 1 },         /* last: the fallback */
};
#define MOUNTS (sizeof mounts / sizeof mounts[0])

/* ---- paths ---- */
int vfs_abspath(const char *cwd, const char *path, char *out, uint32_t size)
{
    uint32_t len = 0;
    const char *src[2] = { path[0] == '/' ? "" : (cwd ? cwd : "/"), path };
    out[0] = 0;
    for (int part = 0; part < 2; ++part) {
        const char *p = src[part];
        while (*p) {
            while (*p == '/') ++p;
            const char *start = p;
            while (*p && *p != '/') ++p;
            uint32_t n = (uint32_t)(p - start);
            if (!n || (n == 1 && start[0] == '.')) continue;
            if (n == 2 && start[0] == '.' && start[1] == '.') {
                while (len && out[len - 1] != '/') --len;
                if (len) --len;              /* drop the slash too */
                out[len] = 0;
                continue;
            }
            if (len + 1 + n + 1 > size) return SYS_ENAMETOOLONG;
            out[len++] = '/';
            memcpy(out + len, start, n);
            len += n;
            out[len] = 0;
        }
    }
    if (!len) { if (size < 2) return SYS_ENAMETOOLONG; out[len++] = '/'; out[len] = 0; }
    return (int)len;
}

static struct mount *lookup(const char *abs, const char **rel)
{
    for (unsigned i = 0; i < MOUNTS; ++i) {
        size_t n = strlen(mounts[i].path);
        if (n == 1) { *rel = abs; return &mounts[i]; }
        if (!strncmp(abs, mounts[i].path, n) && (abs[n] == 0 || abs[n] == '/')) {
            *rel = abs[n] ? abs + n : "/";
            return &mounts[i];
        }
    }
    return 0;
}

/* ---- the fsd task: serialises littlefs and CH376S work on one stack ---- */
struct vreq {
    int (*fn)(struct vreq *r);
    struct vreq *next;
    struct sem done;
    int result;
    struct mount *m;
    struct file *f;
    const char *path, *path2;
    void *buf;
    uint32_t n;
    int32_t off;
    int arg;
};

static struct vreq *queue_head, *queue_tail;
static struct sem queue_ready;

static void fsd_main(void *arg)
{
    (void)arg;
    rootfs_mount(&mounts[MOUNTS - 1]);
    for (;;) {
        sem_take(&queue_ready, SEM_FOREVER);
        for (;;) {
            uint32_t s = irq_save();
            struct vreq *r = queue_head;
            if (r) { queue_head = r->next; if (!queue_head) queue_tail = 0; }
            irq_restore(s);
            if (!r) break;
            r->result = r->fn(r);
            sem_give(&r->done);
        }
    }
}

static int run(struct vreq *r)
{
    if (!r->m->serialized) return r->fn(r);
    r->done = (struct sem){ 0 };
    r->next = 0;
    uint32_t s = irq_save();
    if (queue_tail) queue_tail->next = r; else queue_head = r;
    queue_tail = r;
    irq_restore(s);
    sem_give(&queue_ready);
    sem_take(&r->done, SEM_FOREVER);
    return r->result;
}

void vfs_init(void)
{
    if (!task_spawn("fsd", fsd_main, 0, FSD_STACK, 4, 1)) panic("cannot start fsd");
}

/* ---- operations, executed by run() ---- */
#define OPS(r) ((r)->m->ops)
static int do_open(struct vreq *r) { return OPS(r)->open ? OPS(r)->open(r->m, r->path, r->arg, r->f) : SYS_ENOSYS; }
static int do_close(struct vreq *r) { return OPS(r)->close ? OPS(r)->close(r->f) : 0; }
static int do_read(struct vreq *r) { return OPS(r)->read ? OPS(r)->read(r->f, r->buf, r->n) : SYS_EINVAL; }
static int do_write(struct vreq *r) { return OPS(r)->write ? OPS(r)->write(r->f, r->buf, r->n) : SYS_EROFS; }
static int do_lseek(struct vreq *r) { return OPS(r)->lseek ? OPS(r)->lseek(r->f, r->off, r->arg) : SYS_EINVAL; }
static int do_readdir(struct vreq *r) { return OPS(r)->readdir ? OPS(r)->readdir(r->f, r->buf) : SYS_ENOTDIR; }
static int do_stat(struct vreq *r) { return OPS(r)->stat ? OPS(r)->stat(r->m, r->path, r->buf) : SYS_ENOSYS; }
static int do_mkdir(struct vreq *r) { return OPS(r)->mkdir ? OPS(r)->mkdir(r->m, r->path) : SYS_EROFS; }
static int do_remove(struct vreq *r) { return OPS(r)->remove ? OPS(r)->remove(r->m, r->path) : SYS_EROFS; }
static int do_rename(struct vreq *r) { return OPS(r)->rename ? OPS(r)->rename(r->m, r->path, r->path2) : SYS_ENOSYS; }
static int do_statfs(struct vreq *r) { return OPS(r)->statfs ? OPS(r)->statfs(r->m, r->buf) : SYS_ENOSYS; }

/* Resolve a user path; returns the mount and fills r->path (inside abs). */
static int resolve(const char *path, char *abs, struct vreq *r)
{
    if (!path || !*path) return SYS_ENOENT;
    int n = vfs_abspath(task_self()->cwd, path, abs, NN_PATH_MAX);
    if (n < 0) return n;
    r->m = lookup(abs, &r->path);
    return r->m ? 0 : SYS_ENOENT;
}

static struct file *fd_file(int fd)
{
    struct task *t = task_self();
    return fd >= 0 && fd < TASK_FDS ? t->fds[fd] : 0;
}
struct file *vfs_get(int fd);

int vfs_fd_is_open(int fd) { return fd_file(fd) != 0; }

int vfs_install(struct file *f)
{
    struct task *t = task_self();
    for (int fd = 3; fd < TASK_FDS; ++fd)     /* 0-2 stay with the terminal unless dup2'd */
        if (!t->fds[fd]) { t->fds[fd] = f; return fd; }
    return SYS_EMFILE;
}

struct file *vfs_get(int fd) { return fd_file(fd); }

int vfs_open(const char *path, int flags)
{
    char abs[NN_PATH_MAX];
    struct vreq r = { .fn = do_open, .arg = flags };
    int e = resolve(path, abs, &r);
    if (e) return e;
    struct task *t = task_self();
    int fd = 3;                      /* 0-2 stay with the terminal unless dup2'd */
    while (fd < TASK_FDS && t->fds[fd]) ++fd;
    if (fd == TASK_FDS) return SYS_EMFILE;
    struct file *f = kmalloc(sizeof *f);
    if (!f) return SYS_ENOMEM;
    *f = (struct file){ .mnt = r.m, .flags = (uint16_t)flags, .refs = 1 };
    r.f = f;
    e = run(&r);
    if (e < 0) { kfree(f); return e; }
    t->fds[fd] = f;
    return fd;
}

static int release(struct file *f)
{
    if (--f->refs) return 0;
    struct vreq r = { .fn = do_close, .m = f->mnt, .f = f };
    int e = run(&r);
    kfree(f);
    return e;
}

int vfs_close(int fd)
{
    struct file *f = fd_file(fd);
    if (!f) return SYS_EBADF;
    task_self()->fds[fd] = 0;
    return release(f);
}

void vfs_task_exit(void)
{
    struct task *t = task_self();
    for (int fd = 0; fd < TASK_FDS; ++fd)
        if (t->fds[fd]) vfs_close(fd);
}

int vfs_dup2(int fd, int target)
{
    struct task *t = task_self();
    if (target < 0 || target >= TASK_FDS) return SYS_EBADF;
    struct file *f = 0;
    if (fd >= 0) {
        f = fd_file(fd);
        if (!f) return SYS_EBADF;
        if (fd == target) return target;
        f->refs++;
    }
    struct file *old = t->fds[target];
    t->fds[target] = f;
    if (old) release(old);
    return target;
}

int vfs_read(int fd, void *buf, uint32_t n)
{
    struct file *f = fd_file(fd);
    if (!f) return SYS_EBADF;
    if ((f->flags & O_ACCMODE) == O_WRONLY) return SYS_EBADF;
    struct vreq r = { .fn = do_read, .m = f->mnt, .f = f, .buf = buf, .n = n };
    return run(&r);
}

int vfs_write(int fd, const void *buf, uint32_t n)
{
    struct file *f = fd_file(fd);
    if (!f) return SYS_EBADF;
    if ((f->flags & O_ACCMODE) == O_RDONLY) return SYS_EBADF;
    struct vreq r = { .fn = do_write, .m = f->mnt, .f = f, .buf = (void *)buf, .n = n };
    return run(&r);
}

int vfs_lseek(int fd, int32_t off, int whence)
{
    struct file *f = fd_file(fd);
    if (!f) return SYS_EBADF;
    struct vreq r = { .fn = do_lseek, .m = f->mnt, .f = f, .off = off, .arg = whence };
    return run(&r);
}

int vfs_readdir(int fd, struct nn_dirent *d)
{
    struct file *f = fd_file(fd);
    if (!f) return SYS_EBADF;
    struct vreq r = { .fn = do_readdir, .m = f->mnt, .f = f, .buf = d };
    return run(&r);
}

int vfs_stat(const char *path, struct nn_stat *st)
{
    char abs[NN_PATH_MAX];
    struct vreq r = { .fn = do_stat, .buf = st };
    int e = resolve(path, abs, &r);
    return e ? e : run(&r);
}

int vfs_mkdir(const char *path)
{
    char abs[NN_PATH_MAX];
    struct vreq r = { .fn = do_mkdir };
    int e = resolve(path, abs, &r);
    return e ? e : run(&r);
}

int vfs_unlink(const char *path)
{
    char abs[NN_PATH_MAX];
    struct vreq r = { .fn = do_remove };
    int e = resolve(path, abs, &r);
    if (e) return e;
    if (!strcmp(r.path, "/")) return SYS_EINVAL;    /* a mount point */
    return run(&r);
}

int vfs_rename(const char *from, const char *to)
{
    char abs[NN_PATH_MAX], abs2[NN_PATH_MAX];
    struct vreq r = { .fn = do_rename }, r2 = { 0 };
    int e = resolve(from, abs, &r);
    if (!e) e = resolve(to, abs2, &r2);
    if (e) return e;
    if (r.m != r2.m) return SYS_EXDEV;
    r.path2 = r2.path;
    return run(&r);
}

int vfs_statfs(const char *path, struct nn_statfs *s)
{
    char abs[NN_PATH_MAX];
    struct vreq r = { .fn = do_statfs, .buf = s };
    int e = resolve(path, abs, &r);
    return e ? e : run(&r);
}

int vfs_chdir(const char *path)
{
    char abs[NN_PATH_MAX];
    struct nn_stat st;
    int n = vfs_abspath(task_self()->cwd, path, abs, sizeof abs);
    if (n < 0) return n;
    int e = vfs_stat(abs, &st);
    if (e) return e;
    if (st.type != NN_TYPE_DIR) return SYS_ENOTDIR;
    char *copy = 0;
    if (strcmp(abs, "/")) {
        copy = kmalloc((uint32_t)n + 1);
        if (!copy) return SYS_ENOMEM;
        strcpy(copy, abs);
    }
    struct task *t = task_self();
    kfree(t->cwd);
    t->cwd = copy;
    return 0;
}

int vfs_getcwd(char *buf, uint32_t n)
{
    const char *cwd = task_self()->cwd ? task_self()->cwd : "/";
    uint32_t len = (uint32_t)strlen(cwd);
    if (len + 1 > n) return SYS_ENAMETOOLONG;
    strcpy(buf, cwd);
    return (int)len;
}
