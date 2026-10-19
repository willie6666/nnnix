/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Run the actual shell/parser/control flow against host files and a virtual
 * clock. No MCU drivers or extra command tasks are needed for these tests. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "../../lib/sys.h"
#include "../../user/nsh.h"
#include "../../user/cmds.h"

static union { long double align; unsigned char bytes[2048]; } heap[64];
static unsigned char used[64];
static FILE *fds[8];
static uint32_t now, interrupt_at;
static int fail_seek, fail_rewind, fail_close, no_buffer;
static const char *boot_path;
static const char *lcd_frames;
static FILE *lcd_file;
static int lcd_busy, lcd_fail_write, lcd_fail_seek, lcd_short_write;

static void drop(int fd)
{
    FILE *f = fds[fd];
    fds[fd] = 0;
    if (!f || f == stdin || f == stdout || f == stderr) return;
    for (int i = 0; i < 8; ++i) if (fds[i] == f) return;
    if (f == lcd_file) {
        if (lcd_frames) {
            unsigned char frame[1024] = { 0 };
            rewind(f);
            fread(frame, 1, sizeof frame, f);
            FILE *out = fopen(lcd_frames, "ab");
            if (!out || fwrite(frame, 1, sizeof frame, out) != sizeof frame) abort();
            fclose(out);
        }
        lcd_file = 0;
    }
    fclose(f);
}

intptr_t host_syscall(int n, intptr_t a, intptr_t b, intptr_t c)
{
    switch (n) {
    case SYS_ALLOC:
        if (no_buffer && a == 64) return 0;
        if (a > 2048) abort();
        for (unsigned i = 0; i < 64; ++i) if (!used[i]) { used[i] = 1; return (intptr_t)heap[i].bytes; }
        return 0;
    case SYS_FREE:
        if (!a) return 0;
        for (unsigned i = 0; i < 64; ++i) if ((intptr_t)heap[i].bytes == a && used[i]) { used[i] = 0; return 0; }
        abort();
    case SYS_OPEN:
        if (boot_path && !strcmp((const char *)a, "/etc/rc")) a = (intptr_t)boot_path;
        if (!strcmp((const char *)a, "/dev/lcd")) {
            if (lcd_busy || lcd_file) return SYS_EBUSY;
            for (int i = 3; i < 8; ++i) if (!fds[i]) {
                lcd_file = fds[i] = tmpfile();
                return lcd_file ? i : SYS_EIO;
            }
            return SYS_EMFILE;
        }
        for (int i = 3; i < 8; ++i) if (!fds[i]) {
            if (!strcmp((const char *)a, "/dev/tty")) { fds[i] = stdin; return i; }
            fds[i] = fopen((const char *)a, (b & O_WRONLY) ? (b & O_APPEND ? "ab" : "wb") : "rb");
            return fds[i] ? i : SYS_ENOENT;
        }
        return SYS_EMFILE;
    case SYS_CLOSE:
        if (a < 0 || a >= 8 || !fds[a]) return SYS_EBADF;
        if (a < 3 && fds[a] == (a == 0 ? stdin : a == 1 ? stdout : stderr)) return SYS_EBADF;
        drop((int)a);
        if (a < 3) fds[a] = a == 0 ? stdin : a == 1 ? stdout : stderr;
        return a == 1 && fail_close ? SYS_EIO : 0;
    case SYS_DUP2:
        if (b < 0 || b > 2 || (a >= 0 && (a >= 8 || !fds[a]))) return SYS_EBADF;
        drop((int)b);
        fds[b] = a < 0 ? (b == 0 ? stdin : b == 1 ? stdout : stderr) : fds[a];
        return (int)b;
    case SYS_READ:
        if (a < 0 || a >= 8 || !fds[a]) return SYS_EBADF;
        if (fds[a] == stdin) {
            if (!fgets((char *)b, (int)c + 1, stdin)) return 0;
            if (strchr((char *)b, 0x03)) return SYS_EINTR;
            if (strchr((char *)b, 0x04)) return 0;
            return (intptr_t)strlen((char *)b);
        }
        return (intptr_t)fread((void *)b, 1, (size_t)c, fds[a]);
    case SYS_WRITE:
        if (a < 0 || a >= 8 || !fds[a]) return SYS_EBADF;
        if (fds[a] == lcd_file) {
            if (lcd_fail_write) return SYS_EIO;
            if (lcd_short_write && c > 3) c = 3;
        }
        return (intptr_t)fwrite((const void *)b, 1, (size_t)c, fds[a]);
    case SYS_LSEEK:
        if (fail_seek && c == SEEK_SET) return SYS_EIO;
        if (fail_rewind && c == SEEK_CUR && b < 0) { --fail_rewind; return SYS_EIO; }
        if (fds[a] == lcd_file && lcd_fail_seek) return SYS_EIO;
        if (fseek(fds[a], (long)b, (int)c)) return SYS_EIO;
        return ftell(fds[a]);
    case SYS_STAT: {
        struct stat st;
        if (stat((const char *)a, &st)) return SYS_ENOENT;
        struct nn_stat *out = (struct nn_stat *)b;
        out->size = (uint32_t)st.st_size;
        out->type = S_ISDIR(st.st_mode) ? NN_TYPE_DIR : S_ISREG(st.st_mode) ? NN_TYPE_FILE : NN_TYPE_DEV;
        return 0;
    }
    case SYS_SLEEP: now += (uint32_t)a; return 0;
    case SYS_UPTIME: return now;
    case SYS_TTYINTR:
        if (interrupt_at && now >= interrupt_at) { interrupt_at = 0; return 1; }
        return 0;
    case SYS_UNAME: strcpy((char *)a, "nnnix host simulation"); return 0;
    case SYS_PS: case SYS_READDIR: return 0;
    default: return SYS_ENOSYS;
    }
}

#define STUB(name) int cmd_##name(int argc, char **argv) { (void)argc; (void)argv; return 1; }
STUB(date) STUB(ifconfig)
STUB(dhcp) STUB(service) STUB(netstat) STUB(nc) STUB(ntpdate) STUB(sysctl)

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--boot") && i + 1 < argc) boot_path = argv[++i];
        else if (!strcmp(argv[i], "--fail-seek")) fail_seek = 1;
        else if (!strcmp(argv[i], "--fail-rewind")) fail_rewind = 1;
        else if (!strcmp(argv[i], "--fail-close")) fail_close = 1;
        else if (!strcmp(argv[i], "--no-buffer")) no_buffer = 1;
        else if (!strcmp(argv[i], "--lcd-frames") && i + 1 < argc) lcd_frames = argv[++i];
        else if (!strcmp(argv[i], "--lcd-busy")) lcd_busy = 1;
        else if (!strcmp(argv[i], "--lcd-fail-write")) lcd_fail_write = 1;
        else if (!strcmp(argv[i], "--lcd-fail-seek")) lcd_fail_seek = 1;
        else if (!strcmp(argv[i], "--lcd-short-write")) lcd_short_write = 1;
        else interrupt_at = (uint32_t)strtoul(argv[i], 0, 10);
    }
    fds[0] = stdin; fds[1] = stdout; fds[2] = stderr;
    if (boot_path) {
        nsh_boot(0);
        if (!nsh_boot_done()) return 1;
    } else nsh_main();
    for (unsigned i = 0; i < 64; ++i) if (used[i]) { fprintf(stderr, "allocation leaked\n"); return 1; }
    for (int i = 3; i < 8; ++i) if (fds[i]) { fprintf(stderr, "file descriptor leaked\n"); return 1; }
    return 0;
}
