/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdint.h>
#include "kernel.h"
#include "task.h"
#include "tty.h"
#include "klog.h"
#include "kmem.h"
#include "../lib/fmt.h"
#include "../fs/vfs.h"
#include "../lib/string.h"
#include "../lib/sys.h"
#include "../arch/nuc140/arch.h"
#include "../drivers/rtc.h"
#include "../net/net.h"
#include "../net/telnetd.h"
#include "console.h"
#include "exec.h"
#include "fwupdate.h"
#include "../lib/nnlib.h"

static int32_t sys_exit_k(int32_t code) { task_exit(code); }
static int32_t sys_yield_k(void) { task_yield(); return 0; }
static int32_t sys_sleep_k(uint32_t ms) { task_sleep(ms); return 0; }
static int32_t sys_getpid_k(void) { return task_self()->pid; }
static int32_t sys_uptime_k(void) { return (int32_t)jiffies; }

/* fd 0-2 use the terminal (or the kernel log) until a file is dup2'd onto them. */
static int32_t sys_write_k(int32_t fd, const char *buf, uint32_t len)
{
    if (vfs_fd_is_open(fd)) return vfs_write(fd, buf, len);
    if (fd != 1 && fd != 2) return SYS_EBADF;
    struct tty *t = task_self()->tty;
    if (t) tty_write(t, buf, len);
    else klog_write(buf, len);
    return (int32_t)len;
}

static int32_t sys_read_k(int32_t fd, char *buf, uint32_t len)
{
    if (vfs_fd_is_open(fd)) return vfs_read(fd, buf, len);
    struct tty *t = task_self()->tty;
    if (fd != 0 || !t) return SYS_EBADF;
    return tty_read(t, buf, len);
}

static int32_t sys_ps_k(struct task_info *out, int32_t max, int32_t skip)
{
    return task_list(out, max > TASK_MAX ? TASK_MAX : max, skip);
}

static int32_t sys_meminfo_k(struct kmem_stats *out) { kmem_stats(out); return 0; }
static int32_t sys_klog_k(uint32_t *pos, char *buf, uint32_t len) { return (int32_t)klog_read_from(pos, buf, len); }

static int32_t sys_reboot_k(uint32_t magic)
{
    if (magic != SYS_REBOOT_MAGIC && magic != SYS_REBOOT_CHIP) return SYS_EBADF;
    kprintf("reboot requested by pid %u%s\n", task_self()->pid, magic == SYS_REBOOT_CHIP ? " (chip reset)" : "");
    if (magic == SYS_REBOOT_CHIP) arch_reboot_chip();
    arch_reboot();
}

static int32_t sys_uname_k(char *buf, uint32_t len)
{
    return nn_snprintf(buf, len, "nnnix %s NUC140VE3CN", nnnix_version);
}

static int32_t sys_open_k(const char *path, int32_t flags) { return vfs_open(path, flags); }
static int32_t sys_close_k(int32_t fd) { return vfs_close(fd); }
static int32_t sys_lseek_k(int32_t fd, int32_t off, int32_t whence) { return vfs_lseek(fd, off, whence); }
static int32_t sys_stat_k(const char *path, struct nn_stat *st) { return vfs_stat(path, st); }
static int32_t sys_readdir_k(int32_t fd, struct nn_dirent *d) { return vfs_readdir(fd, d); }
static int32_t sys_mkdir_k(const char *path) { return vfs_mkdir(path); }
static int32_t sys_unlink_k(const char *path) { return vfs_unlink(path); }
static int32_t sys_rename_k(const char *from, const char *to) { return vfs_rename(from, to); }
static int32_t sys_chdir_k(const char *path) { return vfs_chdir(path); }
static int32_t sys_getcwd_k(char *buf, uint32_t n) { return vfs_getcwd(buf, n); }
static int32_t sys_statfs_k(const char *path, struct nn_statfs *s) { return vfs_statfs(path, s); }
static int32_t sys_dup2_k(int32_t fd, int32_t target) { return vfs_dup2(fd, target); }
static int32_t sys_alloc_k(uint32_t n, uint32_t flags)
{
    void *p = (flags & NN_ALLOC_LONG) ? kmalloc_high(n) : kmalloc(n);
    if (p) kmem_set_owner(p, task_self()->pid);   /* freed with the task if the program does not */
    return (int32_t)(uintptr_t)p;
}
static int32_t sys_free_k(void *p) { kfree(p); return 0; }

/* Child programs: the argv copy lives in one block owned by the task. A loaded
 * program (image != 0) gets the library table as a third argument. The task
 * holds a reference to the image; so does any child a loaded program spawns,
 * since it runs code from the same image (task_free drops it). */
struct spawn_block { int (*main)(int, char **); void *image; int argc; char *argv[]; };

static void spawn_entry(void *arg)
{
    struct spawn_block *b = arg;
    if (b->image) task_exit(((int (*)(int, char **, const struct nn_lib *))(void *)b->main)(b->argc, b->argv, &nn_lib));
    task_exit(b->main(b->argc, b->argv));
}

int spawn_child(const struct nn_spawn *req, struct loaded_image *image)
{
    if (!req->main || req->argc < 1 || req->argc > 12 || req->stack < 512u || req->stack > 4096u) return SYS_EINVAL;
    uint32_t size = sizeof(struct spawn_block) + (uint32_t)(req->argc + 1) * sizeof(char *);
    for (int i = 0; i < req->argc; ++i) size += (uint32_t)strlen(req->argv[i]) + 1u;
    struct spawn_block *b = kmalloc(size);
    if (!b) return SYS_ENOMEM;
    b->main = req->main;
    b->image = image;
    b->argc = req->argc;
    char *strings = (char *)&b->argv[req->argc + 1];
    for (int i = 0; i < req->argc; ++i) {
        b->argv[i] = strings;
        strcpy(strings, req->argv[i]);
        strings += strlen(strings) + 1;
    }
    b->argv[req->argc] = 0;
    struct file *in = req->fd_in >= 0 ? vfs_get(req->fd_in) : 0, *out = req->fd_out >= 0 ? vfs_get(req->fd_out) : 0;
    if ((req->fd_in >= 0 && !in) || (req->fd_out >= 0 && !out)) { kfree(b); return SYS_EBADF; }
    /* Set the child up before it can run: no switch happens while interrupts are off. */
    uint32_t s = irq_save();
    struct task *t = task_spawn(b->argv[0], spawn_entry, b, req->stack, task_self()->prio, 0);
    if (t) {
        t->user = 1;
        t->owned = b;
        t->image = image ? image : task_self()->image;
        if (t->image) t->image->refs++;
        if (in) { in->refs++; t->fds[0] = in; }
        if (out) { out->refs++; t->fds[1] = out; }
    }
    irq_restore(s);
    if (!t) { kfree(b); return SYS_ENOMEM; }
    return t->pid;
}

static int32_t sys_spawn_k(const struct nn_spawn *req) { return spawn_child(req, 0); }
static int32_t sys_exec_k(const struct nn_exec *req) { return exec_program(req); }
static int32_t sys_fwupdate_k(const char *path, int32_t flags, struct nn_fwinfo *info)
{
    if (!path && !info && !(flags & (NN_FW_REBOOT | NN_FW_CANCEL))) return SYS_EINVAL;
    return fw_update(path, flags, info);
}

static int32_t sys_wait_k(int32_t pid, int *status, uint32_t timeout_ms)
{
    return task_wait_pid((uint16_t)pid, status, timeout_ms);
}

static int32_t sys_kill_k(int32_t pid) { int r = task_kill((uint16_t)pid); return r == -1 ? SYS_EINVAL : r; }
static int32_t sys_pipe_k(int *fds) { return vfs_pipe(fds); }
static int32_t sys_ttyintr_k(void) { return task_self()->tty ? tty_interrupted(task_self()->tty) : 0; }

static int32_t sys_time_k(void) { return (int32_t)time_now(); }
static int32_t sys_stime_k(uint32_t t)
{
    if (time_set(t)) return SYS_EINVAL;
    kprintf("clock set by pid %u\n", task_self()->pid);
    return 0;
}
static int32_t sys_isatty_k(int32_t fd)
{
    return fd >= 0 && fd <= 2 && !vfs_fd_is_open(fd) && task_self()->tty ? 1 : 0;
}
static int32_t sys_socket_k(int32_t proto) { return sock_create(proto); }
static int32_t sys_connect_k(int32_t fd, const struct nn_sockaddr *a) { return sock_connect(fd, a); }
static int32_t sys_listen_k(int32_t fd, uint32_t port) { return sock_listen(fd, (uint16_t)port); }
static int32_t sys_sockinfo_k(int32_t fd, struct nn_sockinfo *i) { return sock_info(fd, i); }
static int32_t sys_netcfg_k(struct nn_netcfg *c, int32_t op) { return net_config(c, op); }

_Static_assert(CONSOLE_OFF == NN_CONSOLE_OFF && CONSOLE_ASK == NN_CONSOLE_ASK && CONSOLE_ON == NN_CONSOLE_ON,
               "console modes differ from the system call interface");

static int32_t sys_sysctl_k(int32_t key, int32_t value)
{
    int r = SYS_EINVAL;
    switch (key) {
    case NN_CTL_CONSOLE:
        r = console_mode(value);
        break;
    case NN_CTL_IDLE_LOGOUT:
        if (value > 7 * 86400) break;
        if (value >= 0 && (uint32_t)value != tty_idle_logout_s) {
            tty_idle_logout_s = (uint32_t)value;
            kprintf("tty: idle logout %s%d s\n", value ? "after " : "off, ", (int)value);
        }
        r = (int)tty_idle_logout_s;
        break;
    case NN_CTL_TELNET_MAX:
        r = telnetd_max_sessions(value);
        break;
    case NN_CTL_WATCHDOG:
        if (value >= 0 && value != wdt_seconds()) {
            if (wdt_set(value)) break;
            kprintf("watchdog: %s%d s\n", value ? "" : "off, ", (int)value);
        }
        r = wdt_seconds();
        break;
    }
    return r < 0 ? SYS_EINVAL : r;
}

static int32_t sys_mkfs_k(const char *path) { return vfs_mkfs(path); }

static int32_t sys_invalid_k(void) { return SYS_ENOSYS; }

static const uintptr_t table[SYS_COUNT] = {
    [SYS_EXIT] = (uintptr_t)sys_exit_k,
    [SYS_YIELD] = (uintptr_t)sys_yield_k,
    [SYS_SLEEP] = (uintptr_t)sys_sleep_k,
    [SYS_GETPID] = (uintptr_t)sys_getpid_k,
    [SYS_UPTIME] = (uintptr_t)sys_uptime_k,
    [SYS_WRITE] = (uintptr_t)sys_write_k,
    [SYS_READ] = (uintptr_t)sys_read_k,
    [SYS_PS] = (uintptr_t)sys_ps_k,
    [SYS_MEMINFO] = (uintptr_t)sys_meminfo_k,
    [SYS_KLOG] = (uintptr_t)sys_klog_k,
    [SYS_REBOOT] = (uintptr_t)sys_reboot_k,
    [SYS_UNAME] = (uintptr_t)sys_uname_k,
    [SYS_OPEN] = (uintptr_t)sys_open_k,
    [SYS_CLOSE] = (uintptr_t)sys_close_k,
    [SYS_LSEEK] = (uintptr_t)sys_lseek_k,
    [SYS_STAT] = (uintptr_t)sys_stat_k,
    [SYS_READDIR] = (uintptr_t)sys_readdir_k,
    [SYS_MKDIR] = (uintptr_t)sys_mkdir_k,
    [SYS_UNLINK] = (uintptr_t)sys_unlink_k,
    [SYS_RENAME] = (uintptr_t)sys_rename_k,
    [SYS_CHDIR] = (uintptr_t)sys_chdir_k,
    [SYS_GETCWD] = (uintptr_t)sys_getcwd_k,
    [SYS_STATFS] = (uintptr_t)sys_statfs_k,
    [SYS_DUP2] = (uintptr_t)sys_dup2_k,
    [SYS_ALLOC] = (uintptr_t)sys_alloc_k,
    [SYS_FREE] = (uintptr_t)sys_free_k,
    [SYS_SPAWN] = (uintptr_t)sys_spawn_k,
    [SYS_WAIT] = (uintptr_t)sys_wait_k,
    [SYS_KILL] = (uintptr_t)sys_kill_k,
    [SYS_PIPE] = (uintptr_t)sys_pipe_k,
    [SYS_TTYINTR] = (uintptr_t)sys_ttyintr_k,
    [SYS_TIME] = (uintptr_t)sys_time_k,
    [SYS_STIME] = (uintptr_t)sys_stime_k,
    [SYS_ISATTY] = (uintptr_t)sys_isatty_k,
    [SYS_SOCKET] = (uintptr_t)sys_socket_k,
    [SYS_CONNECT] = (uintptr_t)sys_connect_k,
    [SYS_LISTEN] = (uintptr_t)sys_listen_k,
    [SYS_SOCKINFO] = (uintptr_t)sys_sockinfo_k,
    [SYS_NETCFG] = (uintptr_t)sys_netcfg_k,
    [SYS_SYSCTL] = (uintptr_t)sys_sysctl_k,
    [SYS_MKFS] = (uintptr_t)sys_mkfs_k,
    [SYS_EXEC] = (uintptr_t)sys_exec_k,
    [SYS_FWUPDATE] = (uintptr_t)sys_fwupdate_k,
};

/* Called from SVC_Handler with the caller's exception frame:
 * r0 r1 r2 r3 r12 lr pc xpsr. The svc number is the low byte of the
 * instruction before the stacked pc. */
void svc_redirect(uint32_t *frame)
{
    uint8_t n = ((const uint8_t *)(uintptr_t)frame[6])[-2];
    struct task *self = task_self();
    if (self && self->killed && self->user) {       /* a kill takes effect here */
        n = SYS_EXIT;
        frame[0] = 130;
    }
    uintptr_t fn = n < SYS_COUNT && table[n] ? table[n] : (uintptr_t)sys_invalid_k;
    frame[5] = frame[6] | 1u;        /* the kernel function returns to after the svc */
    frame[6] = fn & ~(uintptr_t)1u;  /* exception return resumes in the kernel function */
}
