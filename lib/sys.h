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
    SYS_EXIT   = 0,   /* exit(code)              no return */
    SYS_YIELD  = 1,   /* yield()                 */
    SYS_SLEEP  = 2,   /* sleep(ms)               */
    SYS_GETPID = 3,   /* getpid() -> pid         */
    SYS_UPTIME = 4,   /* uptime() -> ms          */
    SYS_WRITE  = 5,   /* write(fd, buf, len) -> len; fd 1/2 go to the kernel log for now */
    SYS_COUNT
};

#define NN_SVC_CLOBBERS "r1", "r2", "r3", "r12", "lr", "cc", "memory"

#define NN_SYSCALL0(n) ({ \
    register int32_t r0_ __asm("r0"); \
    __asm volatile("svc %1" : "=r"(r0_) : "i"(n) : NN_SVC_CLOBBERS); r0_; })
#define NN_SYSCALL1(n, a) ({ \
    register int32_t r0_ __asm("r0") = (int32_t)(a); \
    __asm volatile("svc %1" : "+r"(r0_) : "i"(n) : NN_SVC_CLOBBERS); r0_; })
#define NN_SYSCALL3(n, a, b, c) ({ \
    register int32_t r0_ __asm("r0") = (int32_t)(a); \
    register int32_t r1_ __asm("r1") = (int32_t)(b); \
    register int32_t r2_ __asm("r2") = (int32_t)(c); \
    __asm volatile("svc %3" : "+r"(r0_), "+r"(r1_), "+r"(r2_) : "i"(n) : "r3", "r12", "lr", "cc", "memory"); r0_; })

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

#endif
