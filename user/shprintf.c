/* SPDX-License-Identifier: GPL-3.0-or-later */
/* A bounded shell printf: escapes can produce NUL; %B emits a numeric byte.
 * Validate the whole invocation before writing, then stream in 32 B chunks. */
#include "shprintf.h"
#include "shtest.h"
#include "cmds.h"
#include "../lib/stdio.h"
#include "../lib/fmt.h"
#include "../lib/sys.h"

struct output { uint8_t bytes[32], used; int error; };

static void flush(struct output *o)
{
    if (!o || !o->used || o->error) return;
    if (nn_write_all(1, o->bytes, o->used) < 0) o->error = 1;
    else if (nn_interrupted()) o->error = 130;
    o->used = 0;
}

static void put(struct output *o, unsigned c)
{
    if (!o || o->error) return;
    o->bytes[o->used++] = (uint8_t)c;
    if (o->used == sizeof o->bytes) flush(o);
}

static int hex(unsigned c)
{
    return c >= '0' && c <= '9' ? (int)(c - '0') : c >= 'a' && c <= 'f' ? (int)(c - 'a' + 10) :
           c >= 'A' && c <= 'F' ? (int)(c - 'A' + 10) : -1;
}

/* p starts after the backslash; leave it just after the escape. */
static int escape(const char **p, unsigned *out)
{
    unsigned c = (unsigned char)*(*p)++;
    switch (c) {
    case '\\': *out = '\\'; return 0;
    case 'a': *out = 7; return 0;
    case 'b': *out = 8; return 0;
    case 'f': *out = 12; return 0;
    case 'n': *out = 10; return 0;
    case 'r': *out = 13; return 0;
    case 't': *out = 9; return 0;
    case 'v': *out = 11; return 0;
    case 'x': {
        int d = hex((unsigned char)**p);
        if (d < 0) return -1;
        *out = (unsigned)d; ++*p;
        d = hex((unsigned char)**p);
        if (d >= 0) { *out = *out * 16u + (unsigned)d; ++*p; }
        return 0;
    }
    default:
        if (c < '0' || c > '7') return -1;
        *out = c - '0';
        /* \0OOO (up to three digits after 0), or \OOO. */
        for (unsigned i = c == '0' ? 0u : 1u; i < 3 && **p >= '0' && **p <= '7'; ++i)
            *out = *out * 8u + (unsigned)(*(*p)++ - '0');
        return *out > 255u ? -1 : 0;
    }
}

static int unsigned_number(const char *p, uint32_t *v)
{
    if (*p == '+') ++p;
    unsigned base = 10;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { base = 16; p += 2; }
    if (!*p) return -1;
    *v = 0;
    for (; *p; ++p) {
        int d = hex((unsigned char)*p);
        if (d < 0 || (unsigned)d >= base || *v > (UINT32_MAX - (unsigned)d) / base) return -1;
        *v = *v * base + (unsigned)d;
    }
    return 0;
}

static int string(struct output *o, const char *p, int escaped)
{
    while (*p) {
        unsigned c = (unsigned char)*p++;
        if (escaped && c == '\\' && escape(&p, &c)) return 2;
        put(o, c);
        if (o && o->error) return o->error;
    }
    return 0;
}

static int render(struct output *o, int argc, char **argv)
{
    int arg = 2;
    do {
        int before = arg;
        const char *p = argv[1];
        while (*p) {
            unsigned c = (unsigned char)*p++;
            if (c == '\\') {
                if (escape(&p, &c)) return 2;
                put(o, c);
            } else if (c != '%') put(o, c);
            else {
                c = (unsigned char)*p++;
                if (c == '%') { put(o, c); continue; }
                if (c != 's' && c != 'b' && c != 'c' && c != 'B' && c != 'd' && c != 'i' && c != 'u' && c != 'x' && c != 'X') return 2;
                int missing = arg == argc;
                const char *value = missing ? "" : argv[arg++];
                if (c == 's' || c == 'b') {
                    int e = string(o, value, c == 'b');
                    if (e) return e;
                } else if (c == 'c') put(o, (unsigned char)value[0]);
                else {
                    char number[12];
                    if (c == 'd' || c == 'i') {
                        int32_t v = 0;
                        if (!missing && sh_number(value, &v)) return 2;
                        nn_snprintf(number, sizeof number, "%d", (int)v);
                    } else {
                        uint32_t v = 0;
                        if ((!missing && unsigned_number(value, &v)) || (c == 'B' && v > 255u)) return 2;
                        if (c == 'B') { put(o, v); continue; }
                        nn_snprintf(number, sizeof number, c == 'u' ? "%u" : c == 'x' ? "%x" : "%X", (unsigned)v);
                    }
                    int e = string(o, number, 0);
                    if (e) return e;
                }
            }
            if (o && o->error) return o->error;
        }
        if (arg == before) break;        /* no conversion: do not loop on unused args */
    } while (arg < argc);
    flush(o);
    return o ? o->error : 0;
}

int cmd_printf(int argc, char **argv)
{
    if (argc < 2) { fputs_fd("usage: printf FORMAT [ARG...]\n", 2); return 2; }
    int e = render(0, argc, argv);
    if (e) { fputs_fd("printf: invalid format, escape or argument\n", 2); return e; }
    if (nn_interrupted()) return 130;
    struct output out = { .used = 0, .error = 0 };
    e = render(&out, argc, argv);
    if (e == 1) fputs_fd("printf: write failed\n", 2);
    return e;
}
