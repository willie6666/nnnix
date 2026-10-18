/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _DEFAULT_SOURCE
#include "../../bin/ed.h"
#include "check.h"
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char out[4096];
static size_t out_len;
static long allocated;               /* blocks not yet released */
static int fail_alloc;
static int fail_after = -1;          /* >= 0: that many more allocations succeed */

void ed_out(const char *s, uint32_t n) { if (out_len + n < sizeof out) { memcpy(out + out_len, s, n); out_len += n; out[out_len] = 0; } }
void *ed_alloc(uint32_t n)
{
    if (fail_alloc || !fail_after) return 0;
    if (fail_after > 0) fail_after--;
    allocated++;
    return malloc(n);
}
void ed_release(void *p) { if (p) { allocated--; free(p); } }
int ed_grow(void *p, uint32_t n) { (void)p; (void)n; return -1; }   /* the copying path */
int ed_open(const char *path, int write) { int fd = open(path, write ? O_WRONLY | O_CREAT | O_TRUNC : O_RDONLY, 0600); return fd < 0 ? -2 : fd; }
int ed_read(int fd, void *buf, uint32_t n) { return (int)read(fd, buf, n); }
int ed_write(int fd, const void *buf, uint32_t n) { return (int)write(fd, buf, n); }
int ed_close(int fd) { return close(fd); }

static struct ed e;
static char dir[] = "/tmp/nnnix-ed-XXXXXX";

/* Feed lines separated by '|'; return what was printed. */
static const char *run(const char *script)
{
    char line[256];
    out_len = 0; out[0] = 0;
    for (int first = 1; first || *script; first = 0) {
        size_t n = strcspn(script, "|");
        memcpy(line, script, n); line[n] = 0;
        script += n + (script[n] == '|');
        if (ed_line(&e, line) == ED_QUIT) ed_out("<quit>", 6);
    }
    return out;
}

static int text_is(const char *expected)
{
    if (e.len == strlen(expected) && !memcmp(e.buf, expected, e.len)) return 1;
    fprintf(stderr, "buffer: \"%.*s\", expected \"%s\"\n", (int)e.len, e.buf ? e.buf : "", expected);
    return 0;
}

static char *path(const char *name) { static char p[64]; snprintf(p, sizeof p, "%s/%s", dir, name); return p; }

static void write_file(const char *name, const char *text)
{
    FILE *f = fopen(path(name), "w"); fputs(text, f); fclose(f);
}

static int file_is(const char *name, const char *expected)
{
    char b[512] = { 0 };
    FILE *f = fopen(path(name), "r");
    if (!f) return 0;
    size_t n = fread(b, 1, sizeof b - 1, f); fclose(f);
    if (n == strlen(expected) && !memcmp(b, expected, n)) return 1;
    fprintf(stderr, "file %s: \"%s\", expected \"%s\"\n", name, b, expected);
    return 0;
}

int main(void)
{
    if (!mkdtemp(dir)) return 1;
    ed_init(&e, 1);

    /* Input, printing, addresses. */
    CHECK(!strcmp(run("a|one|two|three|.|,p"), "one\ntwo\nthree\n"));
    CHECK_EQ_INT(e.lines, 3); CHECK_EQ_INT(e.cur, 3); CHECK(e.dirty);
    CHECK(!strcmp(run("1"), "one\n"));
    CHECK(!strcmp(run(""), "two\n"));
    CHECK(!strcmp(run("-"), "one\n"));
    CHECK(!strcmp(run("$"), "three\n"));
    CHECK(!strcmp(run("2,3n"), "2\ttwo\n3\tthree\n"));
    CHECK(!strcmp(run("1|;p"), "one\none\ntwo\nthree\n"));
    CHECK(!strcmp(run("/thr/="), "3\n"));
    CHECK(!strcmp(run("1|/o/"), "one\ntwo\n"));          /* search starts after the current line */
    CHECK(!strcmp(run("$|/one/="), "three\n1\n"));      /* wraps from the last line to the first */
    CHECK(!strcmp(run("2|/two/="), "two\n2\n"));        /* and round to the current line itself */
    CHECK(!strcmp(run("="), "3\n"));
    CHECK(!strcmp(run(".="), "2\n"));

    /* Insert, append, change, delete, join. */
    CHECK(!strcmp(run("0a|zero|.|1,2p"), "zero\none\n"));
    CHECK(!strcmp(run("2i|half|.|.p"), "half\n"));
    CHECK(text_is("zero\nhalf\none\ntwo\nthree\n"));
    CHECK(!strcmp(run("2,3c|ONE|.|.="), "2\n"));
    CHECK(text_is("zero\nONE\ntwo\nthree\n"));
    CHECK(!strcmp(run("1d|.p"), "ONE\n"));
    CHECK(!strcmp(run("$d|.p"), "two\n"));
    CHECK(!strcmp(run("1,2j|p"), "ONEtwo\n"));
    CHECK_EQ_INT(e.lines, 1);

    /* Substitution. */
    CHECK(!strcmp(run("s/two/ 2/p"), "ONE 2\n"));
    run("a|a-a-a|.");
    CHECK(!strcmp(run("s/-/+/|p"), "a+a-a\n"));
    CHECK(!strcmp(run("s:-:++:g|p"), "a+a++a\n"));
    CHECK(!strcmp(run(",s/a//g|,p"), "ONE 2\n+++\n"));
    CHECK(!strcmp(run("s/x/y/"), "? no match\n"));

    /* Errors. */
    const char *bad[] = { "9p", "0p", "2,1p", "x", "/nothing/", "pq", "s", "s//x/", "s/a/b/z", "1q", "z" };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        run(bad[i]);
        CHECK(!strncmp(out, "? ", 2));
    }
    CHECK(text_is("ONE 2\n+++\n"));

    /* Files, unsaved-change warnings. */
    CHECK(!strcmp(run("w"), "? no file name\n"));
    CHECK(!strcmp(run(strcat(strcpy((char[96]){0}, "w "), path("a.txt"))), ""));
    CHECK(file_is("a.txt", "ONE 2\n+++\n")); CHECK(!e.dirty);
    CHECK(!strcmp(e.file, path("a.txt")));
    run("$a|end|.");
    CHECK(!strcmp(run("q"), "? unsaved changes (repeat to discard)\n"));
    CHECK(!strcmp(run("q"), "<quit>"));
    CHECK(!strcmp(run("2,3w"), "")); CHECK(file_is("a.txt", "+++\nend\n")); CHECK(e.dirty);
    CHECK(!strcmp(run("wq"), "<quit>")); CHECK(file_is("a.txt", "ONE 2\n+++\nend\n"));
    CHECK(!strcmp(run("q"), "<quit>"));

    write_file("b.txt", "x\ny");                     /* no final newline */
    char cmd[96];
    snprintf(cmd, sizeof cmd, "e %s", path("b.txt"));
    CHECK(!strcmp(run(cmd), "")); CHECK(text_is("x\ny\n")); CHECK(!e.dirty); CHECK_EQ_INT(e.cur, 2);
    snprintf(cmd, sizeof cmd, "1r %s", path("a.txt"));
    run(cmd);
    CHECK(text_is("x\nONE 2\n+++\nend\ny\n")); CHECK_EQ_INT(e.cur, 4); CHECK(e.dirty);
    CHECK(!strcmp(run("f"), path("b.txt")) || !strncmp(out, path("b.txt"), strlen(path("b.txt"))));
    snprintf(cmd, sizeof cmd, "e %s", path("missing"));
    CHECK(!strcmp(run(cmd), "? unsaved changes (repeat to discard)\n"));
    CHECK(!strcmp(run(cmd), "? cannot read file\n"));
    CHECK_EQ_INT(e.lines, 0);
    CHECK(!strcmp(run("w"), "")); CHECK(!strcmp(run("w x/y/z"), "? cannot open file\n"));

    /* Byte counts when not silent, paging, empty buffer. */
    e.silent = 0;
    snprintf(cmd, sizeof cmd, "e %s", path("a.txt"));
    CHECK(!strcmp(run(cmd), "14\n"));
    for (int i = 0; i < 9; ++i) run("$a|more|.");
    CHECK(!strcmp(run("1|z"), "ONE 2\n+++\nend\nmore\nmore\nmore\nmore\nmore\n"));
    CHECK_EQ_INT(e.cur, 8);
    run(",d");
    CHECK_EQ_INT(e.lines, 0); CHECK_EQ_INT(e.len, 0); CHECK_EQ_INT(e.cur, 0);
    CHECK(!strcmp(run("p"), "? invalid address\n"));
    CHECK(!strcmp(run("i|first|.|p"), "first\n"));

    /* Out of memory leaves the buffer intact. */
    fail_alloc = 1;
    char big[400]; memset(big, 'b', sizeof big - 1); big[sizeof big - 1] = 0;
    run("a"); out_len = 0; ed_line(&e, big);
    CHECK(!strncmp(out, "? out of memory", 15));
    run(".");
    fail_alloc = 0;
    CHECK(text_is("first\n"));

    /* A substitution that needs more room fails without touching the text,
     * once or with g; reading a file in fails all or nothing. */
    ed_free_buffer(&e);
    char line[130];
    memset(line, 'x', 120); line[120] = 0;
    run("a"); ed_line(&e, line); ed_line(&e, line); run(".");
    CHECK_EQ_INT(e.len, 242);
    char *before = malloc(e.len);
    memcpy(before, e.buf, e.len);
    e.dirty = 0;
    fail_alloc = 1;
    CHECK(!strcmp(run("1s/x/a much longer replacement/"), "? out of memory\n"));   /* 242 + 28 > 256 */
    CHECK(e.len == 242 && !memcmp(e.buf, before, 242));
    CHECK(!strcmp(run(",s/x/yy/g"), "? out of memory\n"));
    CHECK(e.len == 242 && !memcmp(e.buf, before, 242));
    fail_alloc = 0;
    char text[401];
    for (int i = 0; i < 400; ++i) text[i] = i % 40 == 39 ? '\n' : 'r';
    text[400] = 0;
    write_file("c.txt", text);                         /* 4 chunks fit after one growth, the 5th not */
    fail_after = 1;
    int r = ed_load(&e, path("c.txt"), 1);
    fail_after = -1;
    CHECK(r < 0);
    CHECK(e.len == 242 && !memcmp(e.buf, before, 242));
    CHECK_EQ_INT(e.lines, 2); CHECK(!e.dirty);
    CHECK(!strcmp(run("1s/x/y/|1s/x/y/g"), ""));        /* shrinking needs no memory */
    CHECK_EQ_INT(e.len, 242);
    free(before);
    unlink(path("c.txt"));

    ed_free_buffer(&e);
    CHECK_EQ_INT(allocated, 0);
    unlink(path("a.txt")); unlink(path("b.txt")); unlink(path("missing")); rmdir(dir);
    return check_summary("test_ed");
}
