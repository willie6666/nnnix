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
        int w = sys_write(out, buf, (size_t)n);
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

static int copy_file(const char *cmd, const char *src, const char *dst)
{
    char target[NN_PATH_MAX];
    if (is_dir(dst)) {
        uint32_t n = (uint32_t)strlen(dst);
        if (n + 1 + strlen(base_name(src)) + 1 > sizeof target) return fail(cmd, dst, SYS_ENAMETOOLONG);
        strcpy(target, dst);
        if (n && target[n - 1] != '/') target[n++] = '/';
        strcpy(target + n, base_name(src));
        dst = target;
    }
    int in = sys_open(src, O_RDONLY);
    if (in < 0) return fail(cmd, src, in);
    int out = sys_open(dst, O_WRONLY | O_CREAT | O_TRUNC);
    if (out < 0) { sys_close(in); return fail(cmd, dst, out); }
    int e = copy_fd(in, out);
    sys_close(in);
    int c = sys_close(out);
    if (e < 0) return fail(cmd, src, e);
    if (c < 0) return fail(cmd, dst, c);
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
    int e = sys_rename(argv[1], argv[2]);
    if (e == SYS_EXDEV || e == SYS_ENOSYS) {         /* across file systems: copy, then remove */
        if (copy_file("mv", argv[1], argv[2])) return 1;
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
        int w = sys_write(fd, buf, n);
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
