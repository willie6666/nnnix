/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ed.h"
#include "../lib/fmt.h"
#include "../lib/string.h"

#define NONE (-1)

void ed_init(struct ed *e, int silent)
{
    memset(e, 0, sizeof *e);
    e->input_at = -1;
    e->silent = (uint8_t)(silent != 0);
}

void ed_free_buffer(struct ed *e)
{
    ed_release(e->buf);
    e->buf = 0;
    e->len = e->cap = 0;
    e->lines = e->cur = 0;
}

static int fail(const char *why)
{
    ed_out("? ", 2);
    ed_out(why, (uint32_t)strlen(why));
    ed_out("\n", 1);
    return 0;
}

static void print_number(int32_t n, const char *after)
{
    char b[16];
    int k = nn_snprintf(b, sizeof b, "%d%s", (int)n, after);
    ed_out(b, (uint32_t)k);
}

/* ---- the text ---- */

static uint32_t offset_of(const struct ed *e, int32_t line)   /* start of line (1-based); lines+1 -> len */
{
    uint32_t off = 0;
    for (int32_t k = 1; k < line && off < e->len; ++k) {
        while (e->buf[off] != '\n') ++off;
        ++off;
    }
    return off;
}

static uint32_t line_length(const struct ed *e, uint32_t off)
{
    uint32_t n = 0;
    while (e->buf[off + n] != '\n') ++n;
    return n;
}

static int ensure(struct ed *e, uint32_t need)
{
    if (need <= e->cap) return 0;
    uint32_t cap = (need + 255u) & ~255u;
    if (e->buf && !ed_grow(e->buf, cap)) { e->cap = cap; return 0; }   /* no second copy needed */
    char *p = ed_alloc(cap);
    if (!p) return -1;
    if (e->len) memcpy(p, e->buf, e->len);
    ed_release(e->buf);
    e->buf = p;
    e->cap = cap;
    return 0;
}

static int32_t newlines(const char *s, uint32_t n)
{
    int32_t k = 0;
    for (uint32_t i = 0; i < n; ++i) k += s[i] == '\n';
    return k;
}

/* Insert a then b (either may be empty) at off. */
static int insert(struct ed *e, uint32_t off, const char *a, uint32_t na, const char *b, uint32_t nb)
{
    if (ensure(e, e->len + na + nb)) return -1;
    memmove(e->buf + off + na + nb, e->buf + off, e->len - off);
    if (na) memcpy(e->buf + off, a, na);
    if (nb) memcpy(e->buf + off + na, b, nb);
    e->len += na + nb;
    e->lines += newlines(a, na) + newlines(b, nb);
    e->dirty = 1;
    return 0;
}

static void erase(struct ed *e, uint32_t off, uint32_t n)
{
    e->lines -= newlines(e->buf + off, n);
    memmove(e->buf + off, e->buf + off + n, e->len - off - n);
    e->len -= n;
    e->dirty = 1;
}

static void print_lines(struct ed *e, int32_t a, int32_t b, int numbered)
{
    uint32_t off = offset_of(e, a);
    for (int32_t k = a; k <= b; ++k) {
        uint32_t n = line_length(e, off);
        if (numbered) print_number(k, "\t");
        ed_out(e->buf + off, n + 1);
        off += n + 1;
    }
    e->cur = b;
}

/* ---- files ---- */

int ed_load(struct ed *e, const char *path, int32_t after)
{
    int fd = ed_open(path, 0);
    if (fd < 0) return fd;
    uint32_t start = offset_of(e, after + 1), off = start, total = 0;
    int32_t before = e->lines;
    uint8_t dirty = e->dirty;
    char chunk[64];
    int n, ended = 1;
    while ((n = ed_read(fd, chunk, sizeof chunk)) > 0) {
        if (insert(e, off, chunk, (uint32_t)n, 0, 0)) { n = -12; break; }
        off += (uint32_t)n;
        total += (uint32_t)n;
        ended = chunk[n - 1] == '\n';
    }
    ed_close(fd);
    if (n >= 0 && !ended && insert(e, off, "\n", 1, 0, 0)) n = -12;   /* complete the last line */
    if (n < 0) {                                       /* take back what was read: all or nothing */
        if (off > start) erase(e, start, off - start);
        e->dirty = dirty;
        return n;
    }
    e->cur = after + (e->lines - before);
    return (int)total;
}

static int save(struct ed *e, const char *path, int32_t a, int32_t b)
{
    int fd = ed_open(path, 1);
    if (fd < 0) return fail("cannot open file");
    uint32_t from = e->lines ? offset_of(e, a) : 0, to = e->lines ? offset_of(e, b + 1) : 0;
    int w = to > from ? ed_write(fd, e->buf + from, to - from) : 0;
    int c = ed_close(fd);
    if (w < 0 || (uint32_t)w != to - from || c < 0) return fail("write error");
    if (a <= 1 && b == e->lines) e->dirty = 0;
    if (!e->silent) print_number((int32_t)(to - from), "\n");
    return 0;
}

/* ---- commands ---- */

static const char *number(const char *p, int32_t *v)
{
    int32_t n = 0;
    while (*p >= '0' && *p <= '9' && n < 1000000) n = n * 10 + (*p++ - '0');
    *v = n;
    return p;
}

static int32_t search(const struct ed *e, const char *text, uint32_t n)
{
    if (!e->lines) return NONE;
    /* From the line after the current one, wrapping once: the offset moves on
     * with the line, located from the start only at the wrap. */
    int32_t line = e->cur % e->lines + 1;
    uint32_t off = offset_of(e, line);
    for (int32_t k = 0; k < e->lines; ++k) {
        uint32_t len = line_length(e, off);
        for (uint32_t i = 0; i + n <= len; ++i)
            if (!memcmp(e->buf + off + i, text, n)) return line;
        if (line == e->lines) { line = 1; off = 0; }
        else { ++line; off += len + 1u; }
    }
    return NONE;
}

/* One address, or NONE; *bad set on an unusable one. */
static const char *address(struct ed *e, const char *p, int32_t *out, int *bad)
{
    int32_t v = NONE;
    if (*p == '.') { v = e->cur; ++p; }
    else if (*p == '$') { v = e->lines; ++p; }
    else if (*p >= '0' && *p <= '9') p = number(p, &v);
    else if (*p == '/') {
        const char *end = strchr(p + 1, '/');
        uint32_t n = end ? (uint32_t)(end - p - 1) : (uint32_t)strlen(p + 1);
        if (!n) { *bad = 1; return p; }
        v = search(e, p + 1, n);
        if (v == NONE) { *bad = 2; return p; }
        p = end ? end + 1 : p + 1 + n;
    }
    while (*p == '+' || *p == '-') {
        int32_t sign = *p++ == '-' ? -1 : 1, n = 1;
        if (*p >= '0' && *p <= '9') p = number(p, &n);
        v = (v == NONE ? e->cur : v) + sign * n;
    }
    *out = v;
    return p;
}

static int substitute(struct ed *e, int32_t a, int32_t b, const char *p)
{
    char delim = *p;
    if (!delim || delim == ' ' || delim == '\n') return fail("bad substitution");
    const char *old = p + 1, *mid = strchr(old, delim);
    if (!mid || mid == old) return fail("bad substitution");
    const char *rep = mid + 1, *end = strchr(rep, delim);
    uint32_t nold = (uint32_t)(mid - old), nrep = end ? (uint32_t)(end - rep) : (uint32_t)strlen(rep);
    const char *flags = end ? end + 1 : "";
    int global = 0, show = 0;
    for (; *flags; ++flags) {
        if (*flags == 'g') global = 1;
        else if (*flags == 'p') show = 1;
        else return fail("bad substitution");
    }
    /* Count first and make room once, so the command changes all or nothing. */
    uint32_t matches = 0, from = offset_of(e, a), off = from;
    for (int32_t k = a; k <= b; ++k) {
        uint32_t len = line_length(e, off);
        for (uint32_t i = 0; i + nold <= len;) {
            if (memcmp(e->buf + off + i, old, nold)) { ++i; continue; }
            ++matches;
            i += nold;
            if (!global) break;
        }
        off += len + 1;
    }
    if (!matches) return fail("no match");
    if (nrep > nold && ensure(e, e->len + matches * (nrep - nold))) return fail("out of memory");
    int32_t last = NONE;
    off = from;
    for (int32_t k = a; k <= b; ++k) {
        uint32_t len = line_length(e, off), i = 0;
        while (i + nold <= len) {
            if (memcmp(e->buf + off + i, old, nold)) { ++i; continue; }
            erase(e, off + i, nold);
            insert(e, off + i, rep, nrep, 0, 0);       /* the room is there */
            len = len - nold + nrep;
            i += nrep;
            last = k;
            if (!global) break;
        }
        off += len + 1;
    }
    e->cur = last;
    if (show) print_lines(e, last, last, 0);
    return 0;
}

static int unsaved(struct ed *e)
{
    if (!e->dirty || e->warned) return 0;
    e->warned = 1;
    fail("unsaved changes (repeat to discard)");
    return 1;
}

static const char *skip_blanks(const char *p) { while (*p == ' ') ++p; return p; }

int ed_line(struct ed *e, const char *s)
{
    if (e->input_at >= 0) {
        if (s[0] == '.' && !s[1]) {
            e->cur = e->input_at ? e->input_at : (e->lines ? 1 : 0);
            e->input_at = -1;
            return 0;
        }
        if (insert(e, offset_of(e, e->input_at + 1), s, (uint32_t)strlen(s), "\n", 1)) return fail("out of memory");
        e->input_at++;
        return 0;
    }

    int bad = 0, count = 0;
    int32_t a = NONE, b = NONE;
    const char *p = address(e, skip_blanks(s), &a, &bad);
    if (a != NONE) count = 1;
    if (!bad && (*p == ',' || *p == ';')) {
        if (a == NONE) a = *p == ',' ? 1 : e->cur;
        p = address(e, p + 1, &b, &bad);
        if (b == NONE) b = count ? a : e->lines;
        count = 2;
    }
    if (bad) return fail(bad == 2 ? "no match" : "bad address");
    if (count == 1) b = a;
    p = skip_blanks(p);
    char cmd = *p ? *p++ : 0;
    const char *arg = skip_blanks(p);

    /* Defaults, then range checks. */
    int32_t lo = 1;
    switch (cmd) {
    case 0:
        if (!count) a = b = e->cur + 1;
        break;
    case '=': case 'r':
        if (!count) a = b = e->lines;
        lo = 0;
        break;
    case 'w':
        if (!count) { a = e->lines ? 1 : 0; b = e->lines; }
        lo = e->lines ? 1 : 0;
        break;
    case 'a': case 'i':
        if (!count) a = b = e->cur;
        lo = 0;
        break;
    case 'j':
        if (!count) { a = e->cur; b = e->cur + 1; }
        break;
    case 'z':
        if (!count) a = b = e->cur + 1;
        break;
    case 'q': case 'Q': case 'e': case 'f':
        if (count) return fail("unexpected address");
        break;
    default:
        if (!count) a = b = e->cur;
    }
    if (cmd != 'q' && cmd != 'Q' && cmd != 'e' && cmd != 'f')
        if (a < lo || b > e->lines || a > b) return fail("invalid address");

    switch (cmd) {
    case 0:
        print_lines(e, b, b, 0);
        return 0;
    case 'p': case 'n':
        if (*arg) return fail("unexpected text");
        print_lines(e, a, b, cmd == 'n');
        return 0;
    case 'z': {
        int32_t end = a + ED_PAGE - 1 < e->lines ? a + ED_PAGE - 1 : e->lines;
        print_lines(e, a, end, 0);
        return 0;
    }
    case '=':
        print_number(b, "\n");
        return 0;
    case 'a': case 'i': case 'c':
        if (*arg) return fail("unexpected text");
        if (cmd == 'c') {
            uint32_t from = offset_of(e, a);
            erase(e, from, offset_of(e, b + 1) - from);
        }
        e->input_at = cmd == 'a' ? b : a ? a - 1 : 0;
        e->warned = 0;
        return 0;
    case 'd': {
        if (*arg) return fail("unexpected text");
        uint32_t from = offset_of(e, a);
        erase(e, from, offset_of(e, b + 1) - from);
        e->cur = a <= e->lines ? a : e->lines;
        e->warned = 0;
        return 0;
    }
    case 'j': {
        if (*arg) return fail("unexpected text");
        uint32_t off = offset_of(e, a);
        for (int32_t k = a; k < b; ++k) {
            off += line_length(e, off);
            erase(e, off, 1);
        }
        e->cur = a;
        e->warned = 0;
        return 0;
    }
    case 's':
        e->warned = 0;
        return substitute(e, a, b, p);
    case 'w': {
        int quit = *p == 'q';
        const char *name = skip_blanks(quit ? p + 1 : p);
        if (*name && !e->file[0] && strlen(name) < ED_PATH) strcpy(e->file, name);
        if (!*name) name = e->file;
        if (!*name) return fail("no file name");
        if (strlen(name) >= ED_PATH) return fail("file name too long");
        if (save(e, name, a, b)) return 0;
        if (e->dirty && quit) return 0;
        return quit ? ED_QUIT : 0;
    }
    case 'q':
        if (*arg) return fail("unexpected text");
        return unsaved(e) ? 0 : ED_QUIT;
    case 'Q':
        return ED_QUIT;
    case 'f':
        if (*arg) {
            if (strlen(arg) >= ED_PATH) return fail("file name too long");
            strcpy(e->file, arg);
        }
        if (!e->file[0]) return fail("no file name");
        ed_out(e->file, (uint32_t)strlen(e->file));
        ed_out("\n", 1);
        return 0;
    case 'e': case 'r': {
        const char *name = *arg ? arg : e->file;
        if (!*name) return fail("no file name");
        if (strlen(name) >= ED_PATH) return fail("file name too long");
        if (cmd == 'e') {
            if (unsaved(e)) return 0;
            ed_free_buffer(e);
            if (*arg) strcpy(e->file, arg);
        }
        int n = ed_load(e, name, cmd == 'e' ? 0 : b);
        if (n < 0) return fail(n == -12 ? "out of memory" : "cannot read file");
        if (cmd == 'e') { e->dirty = 0; e->warned = 0; }
        if (!e->silent) print_number(n, "\n");
        return 0;
    }
    default:
        return fail("unknown command");
    }
}
