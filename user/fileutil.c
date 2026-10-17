/* SPDX-License-Identifier: GPL-3.0-or-later */
/* File commands for nsh. They use only system calls. */
#include "cmds.h"
#include "../lib/stdio.h"
#include "../lib/string.h"
#include "../lib/sys.h"

#define CHUNK 128

const char *errstr(int e)
{
    switch (e) {
    case SYS_ENOENT: return "no such file or directory";
    case SYS_EIO: return "I/O error";
    case SYS_EBADF: return "bad file descriptor";
    case SYS_ENOMEM: return "out of memory";
    case SYS_EEXIST: return "file exists";
    case SYS_EXDEV: return "different file systems";
    case SYS_ENODEV: return "no device";
    case SYS_ENOTDIR: return "not a directory";
    case SYS_EISDIR: return "is a directory";
    case SYS_EINVAL: return "invalid argument";
    case SYS_EMFILE: return "too many open files";
    case SYS_ENOSPC: return "no space left";
    case SYS_EROFS: return "read-only file system";
    case SYS_ENAMETOOLONG: return "name too long";
    case SYS_ENOSYS: return "not supported";
    case SYS_ENOTEMPTY: return "directory not empty";
    case SYS_EINTR: return "interrupted";
    case SYS_EPIPE: return "broken pipe";
    case SYS_EBUSY: return "busy";
    case SYS_ETIMEDOUT: return "timed out";
    case SYS_ESRCH: return "no such process";
    case SYS_ENOEXEC: return "not an executable (NXE) file";
    default: return "error";
    }
}

int nn_interrupted(void) { return sys_ttyintr() > 0; }

static int fail(const char *cmd, const char *what, int e)
{
    printf("%s: %s: %s\n", cmd, what, errstr(e));
    return 1;
}

static const char *base_name(const char *path)
{
    const char *s = path + strlen(path);
    while (s > path && s[-1] != '/') --s;
    return s;
}

static int is_dir(const char *path)
{
    struct nn_stat st;
    return !sys_stat(path, &st) && st.type == NN_TYPE_DIR;
}

static uint32_t parse_uint(const char *s)
{
    uint32_t v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10u + (uint32_t)(*s++ - '0');
    if (*s == 'k' || *s == 'K') v *= 1024u;
    return v;
}

static int copy_fd(int in, int out)
{
    char buf[CHUNK];
    int n;
    while ((n = sys_read(in, buf, sizeof buf)) > 0) {
        int w = nn_write_all(out, buf, (size_t)n);
        if (w < 0) return w;
        if (nn_interrupted()) return SYS_EINTR;
    }
    return n;
}

static void list_entry(const char *name, const struct nn_stat *st)
{
    if (st->type == NN_TYPE_DIR) printf("%s/\n", name);
    else if (st->type == NN_TYPE_DEV) printf("%s@\n", name);
    else printf("%-12s %8u\n", name, st->size);
}

int cmd_ls(int argc, char **argv)
{
    int rc = 0;
    for (int i = argc > 1 ? 1 : 0; i < argc; ++i) {
        const char *path = argc > 1 ? argv[i] : ".";
        struct nn_stat st;
        int e = sys_stat(path, &st);
        if (e) { rc = fail("ls", path, e); continue; }
        if (st.type != NN_TYPE_DIR) { list_entry(path, &st); continue; }
        int fd = sys_open(path, O_RDONLY | O_DIRECTORY);
        if (fd < 0) { rc = fail("ls", path, fd); continue; }
        struct nn_dirent d;
        while ((e = sys_readdir(fd, &d)) > 0) {
            struct nn_stat est = { d.size, d.type };
            list_entry(d.name, &est);
        }
        if (e < 0) rc = fail("ls", path, e);
        sys_close(fd);
    }
    return rc;
}

int cmd_cat(int argc, char **argv)
{
    if (argc == 1) return copy_fd(0, 1) < 0;
    int rc = 0;
    for (int i = 1; i < argc; ++i) {
        int fd = sys_open(argv[i], O_RDONLY);
        if (fd < 0) { rc = fail("cat", argv[i], fd); continue; }
        int e = copy_fd(fd, 1);
        if (e < 0) rc = fail("cat", argv[i], e);
        sys_close(fd);
    }
    return rc;
}

/* Absolute path with "." and ".." resolved into out (NN_PATH_MAX); 0, or -1
 * if too long. Built in place: the shell's stack is small. */
static int normalize(const char *path, char *out)
{
    uint32_t o = 0;
    if (path[0] != '/') {
        if (sys_getcwd(out, NN_PATH_MAX) < 0) return -1;   /* already normalized */
        o = (uint32_t)strlen(out);
        if (o == 1) o = 0;                                   /* "/" */
    }
    for (const char *p = path; *p;) {
        while (*p == '/') ++p;
        const char *c = p;
        while (*p && *p != '/') ++p;
        uint32_t len = (uint32_t)(p - c);
        if (!len || (len == 1 && c[0] == '.')) continue;
        if (len == 2 && c[0] == '.' && c[1] == '.') { while (o && out[--o] != '/') { } continue; }
        if (o + 1 + len + 1 > NN_PATH_MAX) return -1;
        out[o++] = '/';
        memcpy(out + o, c, len);
        o += len;
    }
    if (!o) out[o++] = '/';
    out[o] = 0;
    return 0;
}

static int under_usb(const char *p) { return !strncmp(p, "/usb", 4) && (!p[4] || p[4] == '/'); }

/* Both name one file: FAT on the USB drive ignores case. */
static int same_file(const char *a, const char *b)
{
    char x[NN_PATH_MAX], y[NN_PATH_MAX];
    if (normalize(a, x) || normalize(b, y)) return 0;
    if (!under_usb(x) || !under_usb(y)) return !strcmp(x, y);
    const char *p = x, *q = y;
    for (; *p && *q; ++p, ++q) {
        char c = *p >= 'a' && *p <= 'z' ? (char)(*p - 32) : *p, d = *q >= 'a' && *q <= 'z' ? (char)(*q - 32) : *q;
        if (c != d) return 0;
    }
    return *p == *q;
}

/* The destination file: dst itself, or src's name inside the directory dst. */
static const char *target_of(const char *src, const char *dst, char *buf)
{
    if (!is_dir(dst)) return dst;
    uint32_t n = (uint32_t)strlen(dst);
    if (n + 1 + strlen(base_name(src)) + 1 > NN_PATH_MAX) return 0;
    strcpy(buf, dst);
    if (n && buf[n - 1] != '/') buf[n++] = '/';
    strcpy(buf + n, base_name(src));
    return buf;
}

static int copy_file(const char *cmd, const char *src, const char *dst)
{
    char target[NN_PATH_MAX];
    const char *to = target_of(src, dst, target);
    if (!to) return fail(cmd, dst, SYS_ENAMETOOLONG);
    /* Opening the destination truncates it: it must not be the source. */
    if (same_file(src, to)) { printf("%s: %s and %s are the same file\n", cmd, src, to); return 1; }
    int in = sys_open(src, O_RDONLY);
    if (in < 0) return fail(cmd, src, in);
    int out = sys_open(to, O_WRONLY | O_CREAT | O_TRUNC);
    if (out < 0) { sys_close(in); return fail(cmd, to, out); }
    int e = copy_fd(in, out);
    sys_close(in);
    int c = sys_close(out);
    if (e < 0) return fail(cmd, src, e);
    if (c < 0) return fail(cmd, to, c);
    return 0;
}

int cmd_cp(int argc, char **argv)
{
    if (argc != 3) { printf("usage: cp SOURCE DEST\n"); return 1; }
    return copy_file("cp", argv[1], argv[2]);
}

int cmd_mv(int argc, char **argv)
{
    if (argc != 3) { printf("usage: mv SOURCE DEST\n"); return 1; }
    char target[NN_PATH_MAX];
    const char *to = target_of(argv[1], argv[2], target);   /* DEST, or SOURCE's name in DEST */
    if (!to) return fail("mv", argv[2], SYS_ENAMETOOLONG);
    if (same_file(argv[1], to)) return 0;           /* moving a file onto itself: nothing to do */
    int e = sys_rename(argv[1], to);
    if (e == SYS_EXDEV || e == SYS_ENOSYS) {         /* across file systems: copy, then remove */
        if (copy_file("mv", argv[1], argv[2])) return 1;   /* the source stays unless the copy closed cleanly */
        e = sys_unlink(argv[1]);
    }
    return e ? fail("mv", argv[1], e) : 0;
}

int cmd_rm(int argc, char **argv)
{
    int rc = 0;
    for (int i = 1; i < argc; ++i) {
        if (is_dir(argv[i])) { rc = fail("rm", argv[i], SYS_EISDIR); continue; }
        int e = sys_unlink(argv[i]);
        if (e) rc = fail("rm", argv[i], e);
    }
    return rc;
}

int cmd_rmdir(int argc, char **argv)
{
    int rc = 0;
    for (int i = 1; i < argc; ++i) {
        if (!is_dir(argv[i])) { rc = fail("rmdir", argv[i], SYS_ENOTDIR); continue; }
        int e = sys_unlink(argv[i]);
        if (e) rc = fail("rmdir", argv[i], e);
    }
    return rc;
}

int cmd_mkdir(int argc, char **argv)
{
    int rc = 0;
    for (int i = 1; i < argc; ++i) {
        int e = sys_mkdir(argv[i]);
        if (e) rc = fail("mkdir", argv[i], e);
    }
    return rc;
}

int cmd_touch(int argc, char **argv)
{
    int rc = 0;
    for (int i = 1; i < argc; ++i) {
        int fd = sys_open(argv[i], O_WRONLY | O_CREAT);
        if (fd < 0) rc = fail("touch", argv[i], fd);
        else sys_close(fd);
    }
    return rc;
}

int cmd_pwd(int argc, char **argv)
{
    char buf[NN_PATH_MAX];
    (void)argc; (void)argv;
    if (sys_getcwd(buf, sizeof buf) >= 0) printf("%s\n", buf);
    return 0;
}

int cmd_cd(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/";
    int e = sys_chdir(path);
    return e ? fail("cd", path, e) : 0;
}

int cmd_df(int argc, char **argv)
{
    static const char *const mounts[] = { "/", "/usb" };
    (void)argc; (void)argv;
    printf("FS      KiB   used   free\n");
    for (unsigned i = 0; i < 2; ++i) {
        struct nn_statfs s;
        if (sys_statfs(mounts[i], &s)) { printf("%-4s      -\n", mounts[i]); continue; }
        uint32_t kib = s.block_size / 256u;          /* quarter KiB units avoid overflow */
        uint32_t total = s.blocks / 4u * kib, avail = s.free_blocks / 4u * kib;
        printf("%-4s %7u %6u %6u\n", mounts[i], total, total - avail, avail);
    }
    return 0;
}

static uint8_t pattern(uint32_t i) { return (uint8_t)(((i * 0x9e3779b1u) >> 24) ^ i); }

/* mkfile NAME SIZE: write SIZE bytes of the test pattern (also a write benchmark). */
int cmd_mkfile(int argc, char **argv)
{
    if (argc != 3) { printf("usage: mkfile NAME SIZE[k]\n"); return 1; }
    uint32_t size = parse_uint(argv[2]), t0 = sys_uptime();
    int fd = sys_open(argv[1], O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return fail("mkfile", argv[1], fd);
    uint8_t buf[CHUNK];
    for (uint32_t pos = 0; pos < size;) {
        uint32_t n = size - pos < CHUNK ? size - pos : CHUNK;
        for (uint32_t k = 0; k < n; ++k) buf[k] = pattern(pos + k);
        int w = nn_write_all(fd, buf, n);
        if (w < 0) { sys_close(fd); return fail("mkfile", argv[1], w); }
        pos += n;
        if (nn_interrupted()) { sys_close(fd); return 130; }
    }
    int e = sys_close(fd);
    if (e) return fail("mkfile", argv[1], e);
    uint32_t ms = sys_uptime() - t0;
    printf("%u bytes in %u ms (%u B/s)\n", size, ms, ms ? size * 10u / ms * 100u : 0);
    return 0;
}

/* sum FILE: CRC-32 (as zlib computes it) and size; also a read benchmark. */
int cmd_sum(int argc, char **argv)
{
    static const uint32_t table[16] = {
        0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac, 0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
        0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c, 0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c,
    };
    int rc = 0;
    for (int i = 1; i < argc; ++i) {
        uint32_t t0 = sys_uptime(), crc = 0xffffffffu, size = 0;
        int fd = sys_open(argv[i], O_RDONLY), n;
        if (fd < 0) { rc = fail("sum", argv[i], fd); continue; }
        uint8_t buf[CHUNK];
        while ((n = sys_read(fd, buf, sizeof buf)) > 0) {
            for (int k = 0; k < n; ++k) {
                crc = (crc >> 4) ^ table[(crc ^ buf[k]) & 0xf];
                crc = (crc >> 4) ^ table[(crc ^ (buf[k] >> 4)) & 0xf];
            }
            size += (uint32_t)n;
            if (nn_interrupted()) { n = SYS_EINTR; break; }
        }
        sys_close(fd);
        if (n < 0) { rc = fail("sum", argv[i], n); continue; }
        uint32_t ms = sys_uptime() - t0;
        printf("%08x %u %s %u ms\n", ~crc, size, argv[i], ms);
    }
    return rc;
}

/* mkfs /: format the onboard flash. Only allowed while "/" is not mounted,
 * i.e. after the boot refused to format a flash that held something else. */
int cmd_mkfs(int argc, char **argv)
{
    if (argc != 2 || strcmp(argv[1], "/")) { printf("usage: mkfs /\n"); return 1; }
    int e = sys_mkfs("/");
    if (e == SYS_EBUSY) { printf("mkfs: / is mounted; it is only formatted when it could not be mounted\n"); return 1; }
    return e ? fail("mkfs", "/", e) : 0;
}

/* fwupdate [-n] FILE: install a firmware image through the LDROM bootloader
 * (-n: at the next boot that goes through it instead of now).
 * fwupdate -c: cancel a staged image. fwupdate: show the update area, the
 * bootloader and CONFIG0. */
int cmd_fwupdate(int argc, char **argv)
{
    static const char *const states[] = { "empty", "staged (installs at the next boot)", "installed" };
    if (argc == 1) {
        struct nn_fwinfo i;
        int e = sys_fwupdate(0, 0, &i);
        if (e) return fail("fwupdate", "status", e);
        printf("update area: %s", states[i.state < 3 ? i.state : 0]);
        if (i.state) printf(", %u bytes, crc %08x", i.size, i.crc);
        printf("\nbootloader: %s\npower-on boot: %s\n", i.bootloader ? "in LDROM" : "none",
               i.boot_ldrom ? "LDROM (bootloader)" : "APROM (CONFIG0)");
        if (i.legacy_fs) printf("/ covers the update area: back up files, then mkfs /\n");
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "-c")) {
        int e = sys_fwupdate(0, NN_FW_CANCEL, 0);
        if (e == SYS_EBUSY) { printf("fwupdate: another update operation is running\n"); return 1; }
        if (e == SYS_ENOSPC) { printf("fwupdate: / covers the update area: nothing to cancel\n"); return 1; }
        if (e) return fail("fwupdate", "cancel", e);
        printf("nothing staged\n");
        return 0;
    }
    int reboot = 1, i = 1;
    if (!strcmp(argv[i], "-n")) { reboot = 0; ++i; }
    if (i + 1 != argc) { printf("usage: fwupdate [-n] FILE | fwupdate -c\n"); return 1; }
    struct nn_fwinfo info;
    sys_fwupdate(0, 0, &info);
    if (reboot && !info.bootloader) { printf("fwupdate: no bootloader in LDROM\n"); return 1; }
    printf("staging %s\n", argv[i]);
    int e = sys_fwupdate(argv[i], reboot ? NN_FW_REBOOT : 0, 0);      /* with -n: stage only */
    if (e == SYS_ENOSPC) { printf("fwupdate: / covers the update area: back up files, then mkfs /\n"); return 1; }
    if (e == SYS_EBUSY) { printf("fwupdate: another update operation is running\n"); return 1; }
    if (e == SYS_ENODEV) { printf("fwupdate: no bootloader in LDROM\n"); return 1; }
    if (e == SYS_ENOEXEC) { printf("fwupdate: %s is not an APROM image\n", argv[i]); return 1; }
    if (e) return fail("fwupdate", argv[i], e);
    printf("staged; it is installed at the next boot through the bootloader\n");
    return 0;
}
