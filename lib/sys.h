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
    SYS_ALLOC   = 24,  /* alloc(bytes, flags) -> pointer, or null; NN_ALLOC_LONG: kept for long */
    SYS_FREE    = 25,  /* free(pointer) */
    SYS_SPAWN   = 26,  /* spawn(const struct nn_spawn *) -> pid */
    SYS_WAIT    = 27,  /* wait(pid, int *status, timeout_ms) -> pid, 0 while running; NN_FOREVER waits */
    SYS_KILL    = 28,  /* kill(pid): the program exits at its next system call */
    SYS_PIPE    = 29,  /* pipe(int fds[2]): read end, write end */
    SYS_TTYINTR = 30,  /* ttyintr() -> 1 if Ctrl-C was typed while nobody read the terminal */
    SYS_TIME    = 31,  /* time() -> seconds since 1970-01-01 UTC */
    SYS_STIME   = 32,  /* stime(seconds): set the clock (and the RTC) */
    SYS_ISATTY  = 33,  /* isatty(fd) -> 1 if fd is the terminal */
    SYS_SOCKET  = 34,  /* socket(NN_SOCK_TCP or NN_SOCK_UDP) -> fd; read/write/close as a file */
    SYS_CONNECT = 35,  /* connect(fd, const struct nn_sockaddr *): TCP starts connecting; UDP sets the peer */
    SYS_LISTEN  = 36,  /* listen(fd, port): TCP accepts one connection on the socket; UDP binds */
    SYS_SOCKINFO = 37, /* sockinfo(fd, struct nn_sockinfo *); fd -1-N reads hardware socket N */
    SYS_NETCFG  = 38,  /* netcfg(struct nn_netcfg *, NN_NET_*) */
    SYS_SYSCTL  = 39,  /* sysctl(NN_CTL_*, value) -> value; a negative value only reads */
    SYS_MKFS    = 40,  /* mkfs("/"): format the flash; only when it did not mount (EBUSY otherwise) */
    SYS_EXEC    = 41,  /* exec(const struct nn_exec *) -> pid: load an NXE program as a child task */
    SYS_FWUPDATE = 42, /* fwupdate(path, NN_FW_*, struct nn_fwinfo *): stage a firmware image, or report */
    SYS_GROW    = 43,  /* grow(pointer, bytes) -> 0 if the block now holds that many bytes in place, else -1 */
    SYS_COUNT
};
#define SYS_REBOOT_MAGIC 0x1d0cu
#define SYS_REBOOT_CHIP  0x1d0du             /* like a power-on reset: boots as CONFIG0 says */

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
/* A child task running main(argc, argv) in this program image. argv is copied.
 * fd_in/fd_out (or -1 for the terminal) become the child's fd 0 and 1. */
struct nn_spawn {
    int (*main)(int argc, char **argv);
    char **argv;
    int argc, fd_in, fd_out;
    uint32_t stack;
};
#define NN_FOREVER 0xffffffffu
/* Like nn_spawn, but the program comes from an NXE file (lib/nnlib.h). */
struct nn_exec {
    const char *path;
    char **argv;
    int argc, fd_in, fd_out;
};
#define SYS_ENOEXEC (-8)                   /* not a valid NXE file */

/* Firmware updates (lib/fwupdate.h): fwupdate(path, 0, 0) copies an APROM
 * image into the update area; with NN_FW_REBOOT it then restarts through the
 * LDROM bootloader, which installs it (one operation: nothing can stage
 * another image in between). fwupdate(0, NN_FW_CANCEL, 0) cancels;
 * fwupdate(0, 0, &info) reports. */
#define NN_FW_REBOOT 1
#define NN_FW_CANCEL 2                     /* fwupdate(0, NN_FW_CANCEL, 0): forget a staged image */
enum { NN_FW_NONE, NN_FW_STAGED, NN_FW_DONE };
struct nn_fwinfo {
    uint32_t state, size, crc;         /* the update area's record */
    uint8_t bootloader;                /* LDROM holds a program */
    uint8_t boot_ldrom;                /* CONFIG0 starts the chip in LDROM */
    uint8_t legacy_fs;                 /* "/" still covers the update area (mkfs / first): ENOSPC */
};

/* Network. Sockets are W5500 hardware sockets; connect and listen do not wait:
 * poll sockinfo until the state is NN_SS_ESTABLISHED (or the socket closed).
 * TCP reads wait for data and return 0 once the peer closed; a UDP read returns
 * one datagram (the rest of a longer one is dropped) and makes its sender the
 * peer for the next write. */
enum { NN_SOCK_TCP = 1, NN_SOCK_UDP = 2 };
struct nn_sockaddr { uint8_t ip[4]; uint16_t port; };
#define NN_SS_CLOSED      0x00         /* states as the W5500 reports them */
#define NN_SS_LISTEN      0x14
#define NN_SS_SYNSENT     0x15
#define NN_SS_ESTABLISHED 0x17
#define NN_SS_CLOSE_WAIT  0x1c
#define NN_SS_UDP         0x22
struct nn_sockinfo {
    uint8_t sn, proto, state, user;    /* proto: NN_SOCK_*, 0 when closed; user: opened by a program */
    uint16_t port, peer_port;
    uint8_t peer[4];
    uint16_t rx, tx_free;              /* bytes waiting, transmit room */
};
/* Run-time settings (sysctl), mostly to give RAM back when a feature is unused. */
enum {
    NN_CTL_CONSOLE,                    /* NN_CONSOLE_*: the LCD/keyboard shell */
    NN_CTL_IDLE_LOGOUT,                /* seconds without input before a terminal logs out; 0 never */
    NN_CTL_TELNET_MAX,                 /* telnet sessions accepted at once, 0-3 */
    NN_CTL_WATCHDOG,                   /* seconds without the kernel daemon before a reset: 0 (off), 2, 7, 26 */
    NN_CTL_CONSOLE_FONT,               /* the LCD's text: 0 normal (21x8), 1 small (32x10) */
    NN_CTL_CONSOLE_HISTORY,            /* bytes of LCD scrollback while the console shell runs, 0-8192 */
    NN_CTL_COUNT
};
enum { NN_CONSOLE_OFF, NN_CONSOLE_ASK, NN_CONSOLE_ON };
#define NN_SVC_TELNETD 0x01
#define NN_SVC_HTTPD   0x02
enum { NN_DHCP_OFF, NN_DHCP_SELECTING, NN_DHCP_REQUESTING, NN_DHCP_BOUND, NN_DHCP_RENEWING, NN_DHCP_FAILED };
enum { NN_NET_GET, NN_NET_SET, NN_NET_DHCP, NN_NET_SERVICES };
struct nn_netcfg {
    uint8_t mac[6], ip[4], mask[4], gw[4], dns[4];   /* NN_NET_SET writes ip, mask, gw and dns */
    uint8_t services;                  /* NN_SVC_*; written by NN_NET_SERVICES */
    uint8_t dhcp;                      /* NN_DHCP_* (NN_NET_DHCP starts a lease request) */
    uint8_t link;                      /* bit0 up, bit1 100M, bit2 full duplex, bit3 chip present */
    uint8_t server[4];                 /* DHCP server */
    uint32_t lease;                    /* seconds left */
};

#define SYS_ESRCH  (-3)
#define SYS_ECHILD (-10)
#define SYS_EPIPE  (-32)
#define SYS_EBUSY  (-16)
#define SYS_ETIMEDOUT (-110)

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
static inline void sys_reboot_chip(void) { (void)NN_SYSCALL1(SYS_REBOOT, SYS_REBOOT_CHIP); }
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
#define NN_ALLOC_LONG 1              /* from the top of the heap, away from short-lived blocks */
static inline void *sys_alloc(size_t n) { return (void *)(uintptr_t)(uint32_t)NN_SYSCALL2(SYS_ALLOC, n, 0); }
static inline void *sys_alloc_long(size_t n) { return (void *)(uintptr_t)(uint32_t)NN_SYSCALL2(SYS_ALLOC, n, NN_ALLOC_LONG); }
static inline void sys_free(void *p) { (void)NN_SYSCALL1(SYS_FREE, (uintptr_t)p); }
static inline int sys_grow(void *p, size_t n) { return NN_SYSCALL2(SYS_GROW, (uintptr_t)p, n); }
static inline int sys_uname(char *buf, size_t len) { return NN_SYSCALL2(SYS_UNAME, (uintptr_t)buf, len); }
static inline int sys_spawn(const struct nn_spawn *s) { return NN_SYSCALL1(SYS_SPAWN, (uintptr_t)s); }
static inline int sys_wait(int pid, int *status, uint32_t timeout_ms)
{
    return NN_SYSCALL3(SYS_WAIT, pid, (uintptr_t)status, timeout_ms);
}
static inline int sys_kill(int pid) { return NN_SYSCALL1(SYS_KILL, pid); }
static inline int sys_pipe(int fds[2]) { return NN_SYSCALL1(SYS_PIPE, (uintptr_t)fds); }
static inline int sys_ttyintr(void) { return NN_SYSCALL0(SYS_TTYINTR); }
static inline uint32_t sys_time(void) { return (uint32_t)NN_SYSCALL0(SYS_TIME); }
static inline int sys_stime(uint32_t t) { return NN_SYSCALL1(SYS_STIME, t); }
static inline int sys_isatty(int fd) { return NN_SYSCALL1(SYS_ISATTY, fd); }
static inline int sys_socket(int proto) { return NN_SYSCALL1(SYS_SOCKET, proto); }
static inline int sys_connect(int fd, const struct nn_sockaddr *a) { return NN_SYSCALL2(SYS_CONNECT, fd, (uintptr_t)a); }
static inline int sys_listen(int fd, uint16_t port) { return NN_SYSCALL2(SYS_LISTEN, fd, port); }
static inline int sys_sockinfo(int fd, struct nn_sockinfo *i) { return NN_SYSCALL2(SYS_SOCKINFO, fd, (uintptr_t)i); }
static inline int sys_netcfg(struct nn_netcfg *c, int op) { return NN_SYSCALL2(SYS_NETCFG, (uintptr_t)c, op); }
static inline int sys_sysctl(int key, int value) { return NN_SYSCALL2(SYS_SYSCTL, key, value); }
static inline int sys_mkfs(const char *path) { return NN_SYSCALL1(SYS_MKFS, (uintptr_t)path); }
static inline int sys_exec(const struct nn_exec *e) { return NN_SYSCALL1(SYS_EXEC, (uintptr_t)e); }
static inline int sys_fwupdate(const char *path, int flags, struct nn_fwinfo *info)
{
    return NN_SYSCALL3(SYS_FWUPDATE, (uintptr_t)path, flags, (uintptr_t)info);
}

#endif
