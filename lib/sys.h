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
    SYS_WRITE   = 5,   /* write(fd, buf, len) -> len; fd 0-2 default to the terminal (kernel log without one) */
    SYS_READ    = 6,   /* read(fd, buf, len) -> len; 0 at end of file; terminal: a line, -EINTR on Ctrl-C */
    SYS_PS      = 7,   /* ps(struct task_info *, max, skip) -> count; skip pages through the list */
    SYS_MEMINFO = 8,   /* meminfo(struct kmem_stats *) */
    SYS_KLOG    = 9,   /* klog(uint32_t *pos, buf, len) -> len; stream the kernel log */
    SYS_REBOOT  = 10,  /* reboot(SYS_REBOOT_MAGIC) */
    SYS_UNAME   = 11,  /* uname(buf, len) -> len */
    SYS_OPEN    = 12,  /* open(path, flags) -> fd */
    SYS_CLOSE   = 13,  /* close(fd) */
    SYS_LSEEK   = 14,  /* lseek(fd, offset, whence) -> position */
    SYS_STAT    = 15,  /* stat(path, struct nn_stat *) */
    SYS_READDIR = 16,  /* readdir(fd, struct nn_dirent *) -> 1, or 0 at the end */
    SYS_MKDIR   = 17,  /* mkdir(path) */
    SYS_UNLINK  = 18,  /* unlink(path): files and empty directories */
    SYS_RENAME  = 19,  /* rename(from, to), within one file system */
    SYS_CHDIR   = 20,  /* chdir(path) */
    SYS_GETCWD  = 21,  /* getcwd(buf, len) -> len */
    SYS_STATFS  = 22,  /* statfs(path, struct nn_statfs *) */
    SYS_DUP2    = 23,  /* dup2(fd, target); fd -1 gives target back to the terminal */
    SYS_COUNT
};
#define SYS_REBOOT_MAGIC 0x1d0cu

/* Error results are negative errno values. */
#define SYS_ENOENT       (-2)
#define SYS_EINTR        (-4)
#define SYS_EIO          (-5)
#define SYS_EBADF        (-9)
#define SYS_ENOMEM       (-12)
#define SYS_EEXIST       (-17)
#define SYS_EXDEV        (-18)
#define SYS_ENODEV       (-19)
#define SYS_ENOTDIR      (-20)
#define SYS_EISDIR       (-21)
#define SYS_EINVAL       (-22)
#define SYS_EMFILE       (-24)
#define SYS_ENOSPC       (-28)
#define SYS_EROFS        (-30)
#define SYS_ENAMETOOLONG (-36)
#define SYS_ENOSYS       (-38)
#define SYS_ENOTEMPTY    (-39)

#define O_RDONLY    0x0000
#define O_WRONLY    0x0001
#define O_RDWR      0x0002
#define O_ACCMODE   0x0003
#define O_CREAT     0x0100
#define O_EXCL      0x0200
#define O_TRUNC     0x0400
#define O_APPEND    0x0800
#define O_DIRECTORY 0x1000
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define NN_PATH_MAX 64
#define NN_NAME_MAX 32
enum { NN_TYPE_FILE = 1, NN_TYPE_DIR = 2, NN_TYPE_DEV = 3 };
struct nn_stat { uint32_t size; uint8_t type; };
struct nn_dirent { char name[NN_NAME_MAX]; uint32_t size; uint8_t type; };
struct nn_statfs { uint32_t block_size, blocks, free_blocks; };

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
static inline int sys_open(const char *path, int flags) { return NN_SYSCALL2(SYS_OPEN, (uintptr_t)path, flags); }
static inline int sys_close(int fd) { return NN_SYSCALL1(SYS_CLOSE, fd); }
static inline int sys_lseek(int fd, int32_t off, int whence) { return NN_SYSCALL3(SYS_LSEEK, fd, off, whence); }
static inline int sys_stat(const char *path, struct nn_stat *st) { return NN_SYSCALL2(SYS_STAT, (uintptr_t)path, (uintptr_t)st); }
static inline int sys_readdir(int fd, struct nn_dirent *d) { return NN_SYSCALL2(SYS_READDIR, fd, (uintptr_t)d); }
static inline int sys_mkdir(const char *path) { return NN_SYSCALL1(SYS_MKDIR, (uintptr_t)path); }
static inline int sys_unlink(const char *path) { return NN_SYSCALL1(SYS_UNLINK, (uintptr_t)path); }
static inline int sys_rename(const char *from, const char *to) { return NN_SYSCALL2(SYS_RENAME, (uintptr_t)from, (uintptr_t)to); }
static inline int sys_chdir(const char *path) { return NN_SYSCALL1(SYS_CHDIR, (uintptr_t)path); }
static inline int sys_getcwd(char *buf, size_t len) { return NN_SYSCALL2(SYS_GETCWD, (uintptr_t)buf, len); }
static inline int sys_statfs(const char *path, struct nn_statfs *s) { return NN_SYSCALL2(SYS_STATFS, (uintptr_t)path, (uintptr_t)s); }
static inline int sys_dup2(int fd, int target) { return NN_SYSCALL2(SYS_DUP2, fd, target); }
static inline int sys_uname(char *buf, size_t len) { return NN_SYSCALL2(SYS_UNAME, (uintptr_t)buf, len); }

#endif
