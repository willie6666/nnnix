/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Small streaming tools: memory use does not grow with file size. */
#include "../user/cmds.h"
#include "../lib/stdio.h"
#include "../lib/string.h"
#include "../lib/sys.h"

struct reader { int fd, pos, len; char buf[64]; };

static int get(struct reader *r)
{
    if (r->pos == r->len) {
        if (nn_interrupted()) return SYS_EINTR;
        int n = sys_read(r->fd, r->buf, sizeof r->buf);
        if (n <= 0) return n < 0 ? n : -1;
        r->pos = 0; r->len = n;
    }
    return (unsigned char)r->buf[r->pos++];
}

static int error(const char *cmd, const char *file, int e)
{
    printf("%s: %s: %s\n", cmd, file, errstr(e));
    return 1;
}

static int write_all(const void *data, int n) { return nn_write_all(1, data, (size_t)n) < 0; }

static int space(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }

int cmd_wc(int argc, char **argv)
{
    int rc = 0;
    for (int i = argc > 1 ? 1 : 0; i < argc; ++i) {
        int fd = i ? sys_open(argv[i], O_RDONLY) : 0;
        if (fd < 0) { rc = error("wc", argv[i], fd); continue; }
        struct reader r = { .fd = fd };
        uint32_t lines = 0, words = 0, bytes = 0;
        int c, word = 0;
        while ((c = get(&r)) >= 0) {
            ++bytes;
            if (c == '\n') ++lines;
            if (!space(c) && !word) ++words;
            word = !space(c);
        }
        if (c < -1) rc = error("wc", i ? argv[i] : "stdin", c);
        else printf("%u %u %u%s%s\n", lines, words, bytes, i ? " " : "", i ? argv[i] : "");
        if (fd) sys_close(fd);
    }
    return rc;
}

static int count_arg(int argc, char **argv, uint32_t *count, int *first)
{
    *count = 10; *first = 1;
    if (argc <= 1 || strcmp(argv[1], "-n")) return 0;
    if (argc < 3 || !argv[2][0]) return 1;
    uint32_t n = 0;
    for (const char *p = argv[2]; *p; ++p) {
        if (*p < '0' || *p > '9' || n > (0xffffffffu - (uint32_t)(*p - '0')) / 10u) return 1;
        n = n * 10u + (uint32_t)(*p - '0');
    }
    *count = n; *first = 3;
    return 0;
}

static int head_fd(int fd, uint32_t lines)
{
    struct reader r = { .fd = fd };
    char out[64];
    int used = 0, c = -1;
    while (lines && (c = get(&r)) >= 0) {
        out[used++] = (char)c;
        if (c == '\n') --lines;
        if (used == (int)sizeof out) { if (write_all(out, used)) return SYS_EIO; used = 0; }
    }
    if (write_all(out, used)) return SYS_EIO;
    return c < -1 ? c : 0;
}

static int tail_fd(int fd, uint32_t lines)
{
    if (!lines) return 0;
    int end = sys_lseek(fd, 0, SEEK_END);
    if (end < 0) return end;
    int at = end, start = 0;
    char buf[64];
    while (at > 0 && !start) {
        int n = at < (int)sizeof buf ? at : (int)sizeof buf;
        at -= n;
        int e = sys_lseek(fd, at, SEEK_SET);
        if (e < 0) return e;
        int got = 0;
        while (got < n) {
            e = sys_read(fd, buf + got, (size_t)(n - got));
            if (e <= 0) return e < 0 ? e : SYS_EIO;
            got += e;
        }
        for (int i = n - 1; i >= 0; --i)
            if (buf[i] == '\n' && at + i != end - 1 && !--lines) { start = at + i + 1; break; }
    }
    int e = sys_lseek(fd, start, SEEK_SET);
    if (e < 0) return e;
    while ((e = sys_read(fd, buf, sizeof buf)) > 0)
        if (write_all(buf, e)) return SYS_EIO;
    return e;
}

static int ends(int argc, char **argv, int tail)
{
    uint32_t n;
    int first;
    if (count_arg(argc, argv, &n, &first)) { printf("usage: %s [-n COUNT] [FILE...]\n", argv[0]); return 1; }
    int rc = 0;
    for (int i = first < argc ? first : 0; i < argc; ++i) {
        int fd = i ? sys_open(argv[i], O_RDONLY) : 0;
        if (fd < 0) { rc = error(argv[0], argv[i], fd); continue; }
        int e = tail ? tail_fd(fd, n) : head_fd(fd, n);
        if (e < 0) rc = error(argv[0], i ? argv[i] : "stdin", e);
        if (fd) sys_close(fd);
        if (!i) break;
    }
    return rc;
}

int cmd_head(int argc, char **argv) { return ends(argc, argv, 0); }
int cmd_tail(int argc, char **argv) { return ends(argc, argv, 1); }

static int contains(const char *line, int n, const char *pattern)
{
    int m = (int)strlen(pattern);
    for (int i = 0; i + m <= n; ++i) if (!memcmp(line + i, pattern, (size_t)m)) return 1;
    return 0;
}

static int grep_fd(int fd, const char *pattern, const char *name, int numbered, int *matched)
{
    struct reader r = { .fd = fd };
    char line[192];
    uint32_t number = 1;
    int n = 0, c;
    while ((c = get(&r)) >= 0) {
        if (n == (int)sizeof line) return SYS_ENAMETOOLONG;
        line[n++] = (char)c;
        if (c != '\n') continue;
        if (contains(line, n - 1, pattern)) {
            *matched = 1;
            if (name) printf("%s:", name);
            if (numbered) printf("%u:", number);
            if (write_all(line, n)) return SYS_EIO;
        }
        ++number; n = 0;
    }
    if (c < -1) return c;
    if (n && contains(line, n, pattern)) {
        *matched = 1;
        if (name) printf("%s:", name);
        if (numbered) printf("%u:", number);
        if (write_all(line, n) || write_all("\n", 1)) return SYS_EIO;
    }
    return 0;
}

int cmd_grep(int argc, char **argv)
{
    int first = 1, numbered = argc > 1 && !strcmp(argv[1], "-n"), matched = 0, rc = 0;
    if (numbered) ++first;
    if (first >= argc) { printf("usage: grep [-n] TEXT [FILE...]\n"); return 2; }
    const char *pattern = argv[first++];
    for (int i = first < argc ? first : 0; i < argc; ++i) {
        int fd = i ? sys_open(argv[i], O_RDONLY) : 0;
        if (fd < 0) { error("grep", argv[i], fd); rc = 2; continue; }
        int e = grep_fd(fd, pattern, argc - first > 1 ? argv[i] : 0, numbered, &matched);
        if (e < 0) { error("grep", i ? argv[i] : "stdin", e); rc = 2; }
        if (fd) sys_close(fd);
        if (!i) break;
    }
    return rc ? rc : !matched;
}

int cmd_hexdump(int argc, char **argv)
{
    if (argc > 2) { printf("usage: hexdump [FILE]\n"); return 1; }
    int fd = argc == 2 ? sys_open(argv[1], O_RDONLY) : 0;
    if (fd < 0) return error("hexdump", argv[1], fd);
    struct reader r = { .fd = fd };
    uint32_t offset = 0;
    unsigned char bytes[16];
    int n = 0, c;
    do {
        c = get(&r);
        if (c >= 0) bytes[n++] = (unsigned char)c;
        if (n && (n == 16 || c < 0)) {
            printf("%08x  ", offset);
            for (int i = 0; i < 16; ++i) {
                if (i < n) printf("%02x ", bytes[i]); else printf("   ");
            }
            printf(" |");
            for (int i = 0; i < n; ++i) printf("%c", bytes[i] >= 32 && bytes[i] < 127 ? bytes[i] : '.');
            printf("|\n");
            offset += (uint32_t)n; n = 0;
        }
    } while (c >= 0);
    if (fd) sys_close(fd);
    return c < -1 ? error("hexdump", argc == 2 ? argv[1] : "stdin", c) : 0;
}
