/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdint.h>
#include "task.h"
#include "klog.h"
#include "../lib/sys.h"
#include "../arch/nuc140/arch.h"

static int32_t sys_exit_k(int32_t code) { task_exit(code); }
static int32_t sys_yield_k(void) { task_yield(); return 0; }
static int32_t sys_sleep_k(uint32_t ms) { task_sleep(ms); return 0; }
static int32_t sys_getpid_k(void) { return task_self()->pid; }
static int32_t sys_uptime_k(void) { return (int32_t)jiffies; }
static int32_t sys_write_k(int32_t fd, const char *buf, uint32_t len)
{
    if (fd != 1 && fd != 2) return -9;           /* EBADF until the VFS exists (M4) */
    klog_write(buf, len);
    return (int32_t)len;
}
static int32_t sys_invalid_k(void) { return -38; }  /* ENOSYS */

static const uintptr_t table[SYS_COUNT] = {
    [SYS_EXIT] = (uintptr_t)sys_exit_k,
    [SYS_YIELD] = (uintptr_t)sys_yield_k,
    [SYS_SLEEP] = (uintptr_t)sys_sleep_k,
    [SYS_GETPID] = (uintptr_t)sys_getpid_k,
    [SYS_UPTIME] = (uintptr_t)sys_uptime_k,
    [SYS_WRITE] = (uintptr_t)sys_write_k,
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
