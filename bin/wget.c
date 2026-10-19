/* SPDX-License-Identifier: GPL-3.0-or-later */
/* wget: fetch a URL over HTTP.
 *   wget [-q] [-S] [-O FILE] URL
 *   -O FILE  where to save it (default: the last part of the path, or
 *            index.html); -O - prints it
 *   -q       no progress messages      -S  show the reply's header lines
 * HTTP/1.0 with Connection: close, so the body ends when the server closes
 * (or after Content-Length); a chunked body is decoded too. A reply whose
 * length is malformed, given twice differently, or in a transfer coding other
 * than chunked is refused. Up to 5 redirects are followed, relative ones
 * resolved against the URL. The exit status is 0 for a 2xx reply saved whole. */
#include "dns.h"
#include "url.h"
#include "../user/cmds.h"
#include "../lib/stdio.h"
#include "../lib/string.h"
#include "../lib/sys.h"

#define URL_MAX 192
#define LINE_MAX 160
#define REDIRECTS 5
#define CONNECT_MS 10000u
#define IDLE_MS 30000u
/* The steps are kept apart (noinline): inlined, all their locals made one
 * 192-byte frame under everything, the usage message included. */

enum { BODY_CLOSE, BODY_LENGTH, BODY_CHUNKED };
#define BAD_CHUNKS 1                     /* body(): a chunked body not as it should be */

struct get {
    char url[URL_MAX];                   /* the current URL; a redirect replaces it */
    struct url u;
    int fd, out, quiet, show;
    uint8_t buf[256];
    uint32_t have, at;
    char line[LINE_MAX];
    int status, body;
    int redirect;                        /* a 3xx's Location: 1 resolved into url, -1 bad */
    uint32_t length, got;
};

static int fail(const char *what, int e)
{
    printf("wget: %s: %s\n", what, errstr(e));
    return 1;
}

/* The next byte from the server: 0-255, -1 when it closed, or an error. */
static int next(struct get *g)
{
    if (g->at < g->have) return g->buf[g->at++];
    for (uint32_t idle = sys_uptime();;) {
        struct nn_sockinfo i;
        if (sys_sockinfo(g->fd, &i)) return SYS_EIO;
        if (i.rx) break;
        if (i.state != NN_SS_ESTABLISHED) return -1;   /* closed, and all read */
        if (nn_interrupted()) return SYS_EINTR;
        if (sys_uptime() - idle > IDLE_MS) return SYS_ETIMEDOUT;
        sys_sleep(10);
    }
    int n = sys_read(g->fd, g->buf, sizeof g->buf);
    if (n <= 0) return n ? n : -1;
    g->have = (uint32_t)n;
    g->at = 1;
    return g->buf[0];
}

/* A header line without its CRLF: its length, or a negative error (-1: the
 * server closed). A longer line is cut to LINE_MAX - 1 characters, and its
 * whole length returned (LINE_MAX or more). */
static int read_line(struct get *g)
{
    int n = 0, c;
    while ((c = next(g)) >= 0 && c != '\n')
        if (c != '\r' && n++ < LINE_MAX - 1) g->line[n - 1] = (char)c;
    g->line[n < LINE_MAX ? n : LINE_MAX - 1] = 0;
    return c < 0 && !n ? c : n;
}

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

/* "Name: value" with this name (any case): the value, blanks around it cut
 * off, or 0. */
static char *field(char *line, const char *name)
{
    size_t n = strlen(name);
    for (size_t i = 0; i < n; ++i) if (lower(line[i]) != name[i]) return 0;
    if (line[n] != ':') return 0;
    char *v = (char *)(uintptr_t)url_skip_blanks(line + n + 1), *end = v + strlen(v);
    while (end > v && (end[-1] == ' ' || end[-1] == '\t')) *--end = 0;
    return v;
}

/* One header line of the reply: 0, or a message for a reply wget refuses. */
static const char *header_line(struct get *g, int len)
{
    char *v;
    uint32_t n;
    const char *end;
    if ((v = field(g->line, "content-length"))) {
        if (len >= LINE_MAX || !(end = url_number(v, 10, &n)) || *end) return "bad Content-Length";
        if (g->body == BODY_LENGTH && n != g->length) return "two different Content-Lengths";
        if (g->body != BODY_CHUNKED) g->body = BODY_LENGTH, g->length = n;   /* chunked wins */
    } else if ((v = field(g->line, "transfer-encoding"))) {
        if (!url_is_word(v, "chunked")) return "a Transfer-Encoding other than chunked";
        g->body = BODY_CHUNKED;
    } else if ((v = field(g->line, "location")) && g->status >= 300 && g->status < 400 && g->status != 304 && !g->redirect) {
        /* url's path is no longer needed: the body of a redirect is not read. */
        g->redirect = len < LINE_MAX && !url_resolve(g->url, URL_MAX, &g->u, v) ? 1 : -1;
    }
    return 0;
}

__attribute__((noinline)) static int send_request(struct get *g)
{
    char port[8] = "";
    if (g->u.port != 80) {
        int n = 0;
        char digits[6];
        for (unsigned v = g->u.port; v; v /= 10u) digits[n++] = (char)('0' + v % 10u);
        port[0] = ':';
        for (int i = 0; i < n; ++i) port[1 + i] = digits[n - 1 - i];
        port[1 + n] = 0;
    }
    const char *path = g->url + g->u.path;
    const char *parts[] = { "GET ", *path == '/' ? "" : "/", path, " HTTP/1.0\r\nHost: ", g->u.host, port,
                            "\r\nUser-Agent: nnnix-wget\r\nAccept: */*\r\nConnection: close\r\n\r\n" };
    for (unsigned i = 0; i < sizeof parts / sizeof parts[0]; ++i) {
        int e = nn_write_all(g->fd, parts[i], strlen(parts[i]));
        if (e < 0) return e;
    }
    return 0;
}

/* Connect, ask, read the status line and the header. 0, or a negative error. */
__attribute__((noinline)) static int request(struct get *g)
{
    uint8_t ip[4];
    if (!g->quiet && dns_parse_ip(g->u.host, ip)) printf("resolving %s\n", g->u.host);
    int e = dns_resolve(g->u.host, ip);
    if (e == SYS_ENOENT) { printf("wget: %s: no such host\n", g->u.host); return 1; }
    if (e == SYS_EINVAL) { printf("wget: no DNS server (ifconfig eth0 ... dns IP)\n"); return 1; }
    if (e) return fail(g->u.host, e);
    /* Off the local network without a gateway the W5500 can only time out. */
    struct nn_netcfg c;
    sys_netcfg(&c, NN_NET_GET);
    int local = 1, gw = c.gw[0] | c.gw[1] | c.gw[2] | c.gw[3];
    for (int i = 0; i < 4; ++i) if ((ip[i] ^ c.ip[i]) & c.mask[i]) local = 0;
    if (!local && !gw) {
        printf("wget: %u.%u.%u.%u is not on the local network, and no gateway is set (ifconfig eth0 ... gw IP)\n",
               ip[0], ip[1], ip[2], ip[3]);
        return 1;
    }
    struct nn_sockaddr a = { .port = g->u.port };
    memcpy(a.ip, ip, 4);
    if (!g->quiet) printf("connecting to %u.%u.%u.%u:%u\n", ip[0], ip[1], ip[2], ip[3], g->u.port);
    g->fd = sys_socket(NN_SOCK_TCP);
    if (g->fd < 0) return fail("socket", g->fd);
    if ((e = sys_connect(g->fd, &a))) return fail("connect", e);
    for (uint32_t start = sys_uptime();;) {
        struct nn_sockinfo i;
        sys_sockinfo(g->fd, &i);
        if (i.state == NN_SS_ESTABLISHED) break;
        if (i.state == NN_SS_CLOSED || sys_uptime() - start > CONNECT_MS) {
            printf("wget: %s: connection refused or timed out\n", g->u.host);
            return 1;
        }
        if (nn_interrupted()) return 130;
        sys_sleep(20);
    }
    if ((e = send_request(g))) return fail("send", e);
    g->have = g->at = 0;
    if ((e = read_line(g)) < 0) return fail("reply", e == -1 ? SYS_EIO : e);
    /* HTTP/x.y NNN [reason] */
    const char *sp = strchr(g->line, ' '), *end;
    uint32_t status;
    if (strncmp(g->line, "HTTP/", 5) || !sp || !(end = url_number(sp + 1, 10, &status)) || end != sp + 4 ||
        (*end && *end != ' ') || status < 100 || status > 599) {
        printf("wget: not an HTTP reply\n");
        return 1;
    }
    g->status = (int)status;
    if (!g->quiet || g->show) printf("%s\n", g->line);
    g->body = BODY_CLOSE;
    g->redirect = 0;
    for (;;) {
        if ((e = read_line(g)) < 0) return fail("reply", e == -1 ? SYS_EIO : e);
        if (!e) return 0;                              /* the blank line: the body follows */
        if (g->show) printf("%s\n", g->line);
        const char *bad = header_line(g, e);
        if (bad) { printf("wget: the reply has %s\n", bad); return 1; }
    }
}

static int put(struct get *g, const uint8_t *p, uint32_t n)
{
    g->got += n;
    return nn_write_all(g->out, p, n) < 0 ? SYS_EIO : 0;
}

/* Data in the buffer: 0, -1 when the server closed, or an error. */
static int fill(struct get *g)
{
    if (g->at < g->have) return 0;
    int c = next(g);
    if (c < 0) return c;
    g->at--;                                           /* next() took a byte: give it back */
    return 0;
}

/* The next chunk's size, after the CRLF that ends the previous chunk's data
 * (first: none). After the last chunk (size 0), its trailer is read too.
 * 0, BAD_CHUNKS, or a negative error (SYS_EIO for a body cut short). */
static int chunk(struct get *g, int first, uint32_t *size)
{
    int c;
    if (!first && (c = read_line(g))) return c == -1 ? SYS_EIO : c < 0 ? c : BAD_CHUNKS;
    if ((c = read_line(g)) < 0) return c == -1 ? SYS_EIO : c;
    const char *end = c < LINE_MAX ? url_number(g->line, 16, size) : 0;
    if (!end || (*(end = url_skip_blanks(end)) && *end != ';')) return BAD_CHUNKS;   /* SIZE [;extensions] */
    while (!*size && (c = read_line(g))) if (c < 0) return c == -1 ? SYS_EIO : c;   /* the trailer */
    return 0;
}

/* The body to g->out: 0, BAD_CHUNKS, or a negative error (SYS_EIO also for a
 * body cut short). */
__attribute__((noinline)) static int body(struct get *g)
{
    int c, chunks = 0;
    uint32_t left = 0;
    for (;;) {
        if (g->body == BODY_CHUNKED && !left) {        /* SIZE CRLF data CRLF ... 0 CRLF trailer CRLF */
            if ((c = chunk(g, !chunks++, &left))) return c;
            if (!left) return 0;
        }
        if (g->body == BODY_LENGTH && g->got >= g->length) return 0;
        if ((c = fill(g)) < 0) return c != -1 ? c : g->body == BODY_CLOSE ? 0 : SYS_EIO;
        uint32_t n = g->have - g->at;
        if (g->body == BODY_LENGTH && n > g->length - g->got) n = g->length - g->got;
        if (g->body == BODY_CHUNKED && n > left) n = left;
        if ((c = put(g, g->buf + g->at, n))) return c;
        g->at += n;
        left -= g->body == BODY_CHUNKED ? n : 0;
    }
}

/* The default file name: the path's last part, without a query, or index.html. */
static void default_name(const char *path, char *name, size_t size)
{
    const char *last = path;
    for (const char *p = path; *p && *p != '?' && *p != '#'; ++p) if (*p == '/') last = p + 1;
    size_t n = 0;
    while (last[n] && last[n] != '?' && last[n] != '#' && n + 1 < size) { name[n] = last[n]; ++n; }
    name[n] = 0;
    if (!n) strcpy(name, "index.html");
}

__attribute__((noinline)) static int run(struct get *g, const char *file)
{
    for (int hop = 0;; ++hop) {
        int e = url_parse(g->url, &g->u);
        if (e == -2) { printf("wget: https is not supported\n"); return 1; }
        if (e) { printf("wget: bad URL: %s\n", g->url); return hop ? 1 : 2; }
        if ((e = request(g))) return e;
        if (g->redirect) {
            sys_close(g->fd);
            g->fd = -1;
            if (hop == REDIRECTS || g->redirect < 0) { printf("wget: too many or bad redirects\n"); return 1; }
            if (!g->quiet) printf("redirected to %s\n", g->url);
            continue;
        }
        break;
    }
    if (g->status < 200 || g->status >= 300) { printf("wget: the server answered %d\n", g->status); return 1; }
    char name[32];
    if (!file) { default_name(g->url + g->u.path, name, sizeof name); file = name; }
    int to_stdout = !strcmp(file, "-");
    g->out = to_stdout ? 1 : sys_open(file, O_WRONLY | O_CREAT | O_TRUNC);
    if (g->out < 0) return fail(file, g->out);
    uint32_t start = sys_uptime();
    int e = body(g);
    uint32_t ms = sys_uptime() - start;
    /* The file is saved only once it is closed: the size or the last data may
     * be written then (review R60). */
    int c = to_stdout ? 0 : sys_close(g->out);
    if (e == SYS_EINTR) return 130;
    if (e) {
        printf("wget: %s after %u bytes\n", e == BAD_CHUNKS ? "a malformed chunked body" : e == SYS_EIO ?
               "connection closed early" : errstr(e), g->got);
        return 1;
    }
    if (c < 0) return fail(file, c);
    if (!g->quiet && !to_stdout)
        printf("saved %s: %u bytes in %u.%01u s\n", file, g->got, ms / 1000u, ms % 1000u / 100u);
    return 0;
}

int cmd_wget(int argc, char **argv)
{
    const char *file = 0, *url = 0;
    int quiet = 0, show = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "-q")) quiet = 1;
        else if (!strcmp(argv[i], "-S")) show = 1;
        else if (!strcmp(argv[i], "-O") && i + 1 < argc) file = argv[++i];
        else if (argv[i][0] != '-' && !url) url = argv[i];
        else url = 0, i = argc;
    }
    if (!url || strlen(url) >= URL_MAX) { printf("usage: wget [-q] [-S] [-O FILE] URL\n"); return 2; }
    struct get *g = sys_alloc(sizeof *g);
    if (!g) return fail("memory", SYS_ENOMEM);
    memset(g, 0, sizeof *g);
    g->fd = -1;
    g->quiet = quiet || (file && !strcmp(file, "-"));
    g->show = show;
    strcpy(g->url, url);
    (void)nn_interrupted();
    int rc = run(g, file);
    if (g->fd >= 0) sys_close(g->fd);
    sys_free(g);
    return rc;
}
