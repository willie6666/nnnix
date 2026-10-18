/* SPDX-License-Identifier: GPL-3.0-or-later */
/* HTTP URLs (RFC 3986, the http scheme only) and header numbers: url.h. */
#include "url.h"
#include "../lib/string.h"

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

/* s starts with p, ignoring case. */
static int starts(const char *s, const char *p)
{
    for (; *p; ++s, ++p) if (lower(*s) != *p) return 0;
    return 1;
}

const char *url_skip_blanks(const char *s)
{
    while (*s == ' ' || *s == '\t') ++s;
    return s;
}

int url_is_word(const char *s, const char *word)
{
    return starts(s, word) && !*url_skip_blanks(s + strlen(word));
}

const char *url_number(const char *s, int base, uint32_t *v)
{
    const char *start = s;
    for (*v = 0;; ++s) {
        int c = lower(*s), d = c >= '0' && c <= '9' ? c - '0' : base == 16 && c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (d < 0) return s > start ? s : 0;
        if (*v > (0xffffffffu - (uint32_t)d) / (uint32_t)base) return 0;
        *v = *v * (uint32_t)base + (uint32_t)d;
    }
}

/* "." and ".." segments out of the path at p (which starts with '/', and ends
 * at its end or a '?'), in place. */
static void clean_path(char *p)
{
    char *out = p, *in = p;
    while (*in == '/') {
        char *seg = in + 1, *end = seg;
        while (*end && *end != '/' && *end != '?') ++end;
        size_t n = (size_t)(end - seg);
        if (n == 1 && seg[0] == '.') {
            if (*end != '/') *out++ = '/';
        } else if (n == 2 && seg[0] == '.' && seg[1] == '.') {
            while (out > p && *--out != '/') { }       /* the last segment out */
            if (*end != '/') *out++ = '/';
        } else {
            memmove(out, in, (size_t)(end - in));
            out += end - in;
        }
        in = end;
    }
    memmove(out, in, strlen(in) + 1);
}

int url_parse(char *url, struct url *u)
{
    char *hash = strchr(url, '#');
    if (hash) *hash = 0;
    char *p = url;
    if (starts(p, "https://")) return -2;
    if (starts(p, "http://")) p += 7;
    char *end = p;
    while (*end && *end != '/' && *end != ':' && *end != '?') ++end;
    if (end == p || end - p >= URL_HOST_MAX) return -1;
    memcpy(u->host, p, (size_t)(end - p));
    u->host[end - p] = 0;
    u->port = 80;
    if (*end == ':') {
        uint32_t v;
        const char *after = url_number(end + 1, 10, &v);
        if (!after || !v || v > 65535u) return -1;
        u->port = (uint16_t)v;
        end = (char *)(uintptr_t)after;
    }
    if (*end && *end != '/' && *end != '?') return -1;
    u->path = (uint16_t)(end - url);
    if (*end == '/') clean_path(end);
    return 0;
}

int url_resolve(char *url, size_t size, const struct url *u, const char *ref)
{
    const char *p = ref, *glue = "";
    size_t keep;
    while (*p && *p != ':' && *p != '/' && *p != '?' && *p != '#') ++p;
    if (!*ref) return -1;
    if (*p == ':' && p > ref) keep = 0;                          /* scheme:... */
    else if (ref[0] == '/' && ref[1] == '/') { keep = 0; glue = "http:"; }
    else {
        const char *path = url + u->path;
        size_t len = 0;
        while (path[len] && path[len] != '?') ++len;
        if (ref[0] == '/') keep = u->path;
        else if (ref[0] == '?') keep = u->path + len;
        else if (ref[0] == '#') keep = u->path + strlen(path);
        else {                                                   /* beside the last part of the path */
            keep = u->path;
            for (size_t i = 0; i < len; ++i) if (path[i] == '/') keep = u->path + i + 1;
        }
        if (keep == u->path && ref[0] != '/') glue = "/";        /* no path: the root's */
    }
    size_t g = strlen(glue), n = strlen(ref);
    if (keep + g + n >= size) return -1;
    memcpy(url + keep, glue, g);
    memcpy(url + keep + g, ref, n + 1);
    return 0;
}
