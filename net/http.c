/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "http.h"
#include "../lib/string.h"

static int equal(const char *a, const char *b)
{
    while (*a && *b) {
        char c = *a++;
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (c != *b++) return 0;
    }
    return *a == *b;
}

/* Only literal paths below /home or /bin (programs). Reject ambiguous
 * encodings and dot components before VFS normalisation; uploads cannot reach
 * /etc, /dev, /proc or /usb. */
static int path_ok(const char *s)
{
    size_t skip = !strncmp(s, "/home/", 6) ? 6 : !strncmp(s, "/bin/", 5) ? 5 : 0;
    if (!skip || !s[skip] || strlen(s) >= HTTP_PATH) return 0;
    for (const char *p = s + skip; *p;) {
        const char *start = p;
        while (*p && *p != '/') {
            char c = *p++;
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return 0;
        }
        size_t n = (size_t)(p - start);
        if (!n || n > 31 || (n == 1 && *start == '.') ||
            (n == 2 && start[0] == '.' && start[1] == '.') || !strncmp(start, ".http-", 6)) return 0;
        if (*p && !*++p) return 0;
    }
    return 1;
}

static int field_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
        || (c && strchr("!#$%&'*+-.^_`|~", c));
}

int http_parse_line(struct http_request *r, char *line)
{
    if (!r->started) {
        char *path = strchr(line, ' ');
        if (!path) return -400;
        *path++ = 0;
        if (strcmp(line, "GET") && strcmp(line, "PUT")) return -405;
        r->put = !strcmp(line, "PUT");
        char *version = strchr(path, ' ');
        if (!version) return -400;
        *version++ = 0;
        if (strcmp(version, "HTTP/1.0") && strcmp(version, "HTTP/1.1")) return -400;
        if (!path_ok(path)) return -403;
        strcpy(r->path, path);
        r->http11 = !strcmp(version, "HTTP/1.1");
        r->started = 1;
        return 0;
    }
    if (!*line) {
        if (r->http11 && !r->host) return -400;
        if (r->put && !r->has_length) return -411;
        if (!r->put && r->length) return -400;
        return 1;
    }
    char *v = strchr(line, ':');
    if (!v || v == line) return -400;
    for (char *p = line; p < v; ++p) if (!field_char(*p)) return -400;
    *v++ = 0;
    while (*v == ' ' || *v == '\t') ++v;
    char *end = v + strlen(v);
    while (end > v && (end[-1] == ' ' || end[-1] == '\t')) *--end = 0;
    for (char *p = v; *p; ++p) if ((unsigned char)*p < 32 && *p != '\t') return -400;
    if (equal(line, "content-length")) {
        if (r->has_length || !*v) return -400;
        uint32_t n = 0;
        for (; *v; ++v) {
            if (*v < '0' || *v > '9') return -400;
            if (n > (HTTP_MAX_BODY - (uint32_t)(*v - '0')) / 10u) return -413;
            n = n * 10u + (uint32_t)(*v - '0');
        }
        r->length = n; r->has_length = 1;
    } else if (equal(line, "transfer-encoding")) return -501;
    else if (equal(line, "content-range")) return -400;
    else if (equal(line, "host")) {
        if (r->host || !*v || strchr(v, ' ') || strchr(v, '\t')) return -400;
        r->host = 1;
    } else if (equal(line, "expect")) {
        if (!equal(v, "100-continue")) return -417;
        r->expect = 1;
    }
    return 0;
}
