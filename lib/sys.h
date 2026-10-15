/* SPDX-License-Identifier: GPL-3.0-or-later */
/* nnnix system calls (user side). Programs reach the kernel only through these
 * svc stubs, so built-in and (from M6) loaded programs share one ABI.
 * Arguments in r0-r3, result in r0; the kernel function runs in the caller's
 * task context, so r1-r3, r12 and lr are clobbered like a normal call. */
#ifndef NNNIX_SYS_H
#define NNNIX_SYS_H
#include <stddef.h>
#include <stdint.h>

enum {
    SYS_EXIT    = 0,   /* exit(code)                 no return */
    SYS_YIELD   = 1,   /* yield()                    */
    SYS_SLEEP   = 2,   /* sleep(ms)                  */
    SYS_GETPID  = 3,   /* getpid() -> pid            */
    SYS_UPTIME  = 4,   /* uptime() -> ms             */
    SYS_WRITE   = 5,   /* write(fd, buf, len) -> len; fd 1/2: terminal, or the kernel log without one */
    SYS_READ    = 6,   /* read(fd, buf, len) -> len; fd 0: terminal line; 0 at end of file, -EINTR on Ctrl-C */
    SYS_PS      = 7,   /* ps(struct task_info *, max, skip) -> count; skip pages through the list */
    SYS_MEMINFO = 8,   /* meminfo(struct kmem_stats *) */
    SYS_KLOG    = 9,   /* klog(uint32_t *pos, buf, len) -> len; stream the kernel log */
    SYS_REBOOT  = 10,  /* reboot(SYS_REBOOT_MAGIC) */
    SYS_UNAME   = 11,  /* uname(buf, len) -> len */
    SYS_COUNT
};
#define SYS_REBOOT_MAGIC 0x1d0cu
#define SYS_EBADF  (-9)
#define SYS_EINTR  (-4)
#define SYS_ENOSYS (-38)

#define NN_SVC_CLOBBERS "r1", "r2", "r3", "r12", "lr", "cc", "memory"

#define NN_SYSCALL0(n) ({ \
    register int32_t r0_ __asm("r0"); \
    __asm volatile("svc %1" : "=r"(r0_) : "i"(n) : NN_SVC_CLOBBERS); r0_; })
#define NN_SYSCALL1(n, a) ({ \
    register int32_t r0_ __asm("r0") = (int32_t)(a); \
    __asm volatile("svc %1" : "+r"(r0_) : "i"(n) : NN_SVC_CLOBBERS); r0_; })
#define NN_SYSCALL2(n, a, b) ({ \
    register int32_t r0_ __asm("r0") = (int32_t)(a); \
    register int32_t r1_ __asm("r1") = (int32_t)(b); \
    __asm volatile("svc %2" : "+r"(r0_), "+r"(r1_) : "i"(n) : "r2", "r3", "r12", "lr", "cc", "memory"); r0_; })
#define NN_SYSCALL3(n, a, b, c) ({ \
    register int32_t r0_ __asm("r0") = (int32_t)(a); \
    register int32_t r1_ __asm("r1") = (int32_t)(b); \
    register int32_t r2_ __asm("r2") = (int32_t)(c); \
    __asm volatile("svc %3" : "+r"(r0_), "+r"(r1_), "+r"(r2_) : "i"(n) : "r3", "r12", "lr", "cc", "memory"); r0_; })

struct task_info;
struct kmem_stats;

static inline __attribute__((noreturn)) void sys_exit(int code)
{
    NN_SYSCALL1(SYS_EXIT, code);
    __builtin_unreachable();
}
static inline void sys_yield(void) { (void)NN_SYSCALL0(SYS_YIELD); }
static inline void sys_sleep(uint32_t ms) { (void)NN_SYSCALL1(SYS_SLEEP, ms); }
static inline int sys_getpid(void) { return NN_SYSCALL0(SYS_GETPID); }
static inline uint32_t sys_uptime(void) { return (uint32_t)NN_SYSCALL0(SYS_UPTIME); }
static inline int sys_write(int fd, const void *buf, size_t len)
{
    return NN_SYSCALL3(SYS_WRITE, fd, (uintptr_t)buf, len);
}
static inline int sys_read(int fd, void *buf, size_t len)
{
    return NN_SYSCALL3(SYS_READ, fd, (uintptr_t)buf, len);
}
static inline int sys_ps(struct task_info *out, int max, int skip)
{
    return NN_SYSCALL3(SYS_PS, (uintptr_t)out, max, skip);
}
static inline void sys_meminfo(struct kmem_stats *out) { (void)NN_SYSCALL1(SYS_MEMINFO, (uintptr_t)out); }
static inline int sys_klog(uint32_t *pos, char *buf, size_t len)
{
    return NN_SYSCALL3(SYS_KLOG, (uintptr_t)pos, (uintptr_t)buf, len);
}
static inline void sys_reboot(void) { (void)NN_SYSCALL1(SYS_REBOOT, SYS_REBOOT_MAGIC); }
static inline int sys_uname(char *buf, size_t len) { return NN_SYSCALL2(SYS_UNAME, (uintptr_t)buf, len); }

#endif
