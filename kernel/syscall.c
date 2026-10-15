/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdint.h>
#include "kernel.h"
#include "task.h"
#include "tty.h"
#include "klog.h"
#include "kmem.h"
#include "../lib/fmt.h"
#include "../lib/sys.h"
#include "../arch/nuc140/arch.h"

static int32_t sys_exit_k(int32_t code) { task_exit(code); }
static int32_t sys_yield_k(void) { task_yield(); return 0; }
static int32_t sys_sleep_k(uint32_t ms) { task_sleep(ms); return 0; }
static int32_t sys_getpid_k(void) { return task_self()->pid; }
static int32_t sys_uptime_k(void) { return (int32_t)jiffies; }

static int32_t sys_write_k(int32_t fd, const char *buf, uint32_t len)
{
    if (fd != 1 && fd != 2) return SYS_EBADF;   /* until the VFS exists (M4) */
    struct tty *t = task_self()->tty;
    if (t) tty_write(t, buf, len);
    else klog_write(buf, len);
    return (int32_t)len;
}

static int32_t sys_read_k(int32_t fd, char *buf, uint32_t len)
{
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
    if (magic != SYS_REBOOT_MAGIC) return SYS_EBADF;
    kprintf("reboot requested by pid %u\n", task_self()->pid);
    arch_reboot();
}

static int32_t sys_uname_k(char *buf, uint32_t len)
{
    return nn_snprintf(buf, len, "nnnix %s NUC140VE3CN", nnnix_version);
}

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
};

/* Called from SVC_Handler with the caller's exception frame:
 * r0 r1 r2 r3 r12 lr pc xpsr. The svc number is the low byte of the
 * instruction before the stacked pc. */
void svc_redirect(uint32_t *frame)
{
    uint8_t n = ((const uint8_t *)(uintptr_t)frame[6])[-2];
    uintptr_t fn = n < SYS_COUNT && table[n] ? table[n] : (uintptr_t)sys_invalid_k;
    frame[5] = frame[6] | 1u;        /* the kernel function returns to after the svc */
    frame[6] = fn & ~(uintptr_t)1u;  /* exception return resumes in the kernel function */
}
