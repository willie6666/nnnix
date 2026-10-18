/* SPDX-License-Identifier: GPL-3.0-or-later */
/* A DNS stub resolver (RFC 1035): one A question, recursion desired, over UDP
 * to the configured server. Also the command `host NAME`. */
#include "dns.h"
#include "../user/cmds.h"
#include "../lib/stdio.h"
#include "../lib/string.h"
#include "../lib/sys.h"

#define DNS_PORT 53
#define MSG 512                              /* the most a UDP answer may be */
#define TYPE_A 1
#define TYPE_CNAME 5

int dns_parse_ip(const char *s, uint8_t ip[4])
{
    for (int i = 0; i < 4; ++i) {
        unsigned v = 0, digits = 0;
        while (*s >= '0' && *s <= '9' && digits < 4) { v = v * 10u + (unsigned)(*s++ - '0'); ++digits; }
        if (!digits || v > 255 || *s != (i < 3 ? '.' : 0)) return -1;
        ip[i] = (uint8_t)v;
        if (i < 3) ++s;
    }
    return 0;
}

/* The question: id, flags RD, one question of NAME, type A, class IN. */
static int question(uint8_t *m, uint16_t id, const char *name)
{
    static const uint8_t head[10] = { 0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, 0 };
    m[0] = (uint8_t)(id >> 8);
    m[1] = (uint8_t)id;
    memcpy(m + 2, head, sizeof head);
    int n = 12;
    for (const char *p = name; *p;) {
        const char *dot = strchr(p, '.');
        int len = dot ? (int)(dot - p) : (int)strlen(p);
        if (len < 1 || len > 63 || n + len + 6 > MSG) return -1;
        m[n++] = (uint8_t)len;
        memcpy(m + n, p, (size_t)len);
        n += len;
        p += len;
        if (*p) ++p;
    }
    m[n++] = 0;
    m[n++] = 0; m[n++] = TYPE_A;
    m[n++] = 0; m[n++] = 1;
    return n;
}

/* Past a name at m[at] (labels or a compression pointer): the next offset, or -1. */
static int skip_name(const uint8_t *m, int len, int at)
{
    while (at < len) {
        if (m[at] == 0) return at + 1;
        if ((m[at] & 0xc0u) == 0xc0u) return at + 2 <= len ? at + 2 : -1;
        at += m[at] + 1;
    }
    return -1;
}

static int answer(const uint8_t *m, int len, uint16_t id, uint8_t ip[4])
{
    if (len < 12 || m[0] != (uint8_t)(id >> 8) || m[1] != (uint8_t)id || !(m[2] & 0x80u)) return 1;   /* not ours */
    int rcode = m[3] & 0x0f;
    if (rcode == 3) return SYS_ENOENT;                 /* NXDOMAIN */
    if (rcode) return SYS_EIO;
    int qd = m[4] << 8 | m[5], an = m[6] << 8 | m[7], at = 12;
    for (int i = 0; i < qd && at >= 0; ++i)
        if ((at = skip_name(m, len, at)) >= 0) at += 4;          /* type, class */
    for (int i = 0; i < an && at >= 0 && at <= len; ++i) {
        at = skip_name(m, len, at);
        if (at < 0 || at + 10 > len) break;
        int type = m[at] << 8 | m[at + 1], rdlen = m[at + 8] << 8 | m[at + 9];
        at += 10;
        if (at + rdlen > len) break;
        /* CNAMEs come first in a recursive answer, then the target's A records. */
        if (type == TYPE_A && rdlen == 4 && m[at - 8] == 0 && m[at - 7] == 1) { memcpy(ip, m + at, 4); return 0; }
        at += rdlen;
    }
    return SYS_ENOENT;
}

int dns_resolve(const char *name, uint8_t ip[4])
{
    if (!dns_parse_ip(name, ip)) return 0;
    struct nn_netcfg c;
    sys_netcfg(&c, NN_NET_GET);
    if (!(c.dns[0] | c.dns[1] | c.dns[2] | c.dns[3])) return SYS_EINVAL;
    uint8_t *m = sys_alloc(MSG);
    if (!m) return SYS_ENOMEM;
    int fd = sys_socket(NN_SOCK_UDP), e = fd;
    struct nn_sockaddr a = { .port = DNS_PORT };
    memcpy(a.ip, c.dns, 4);
    uint16_t id = (uint16_t)(sys_uptime() * 2654435761u >> 16);
    int qlen = fd >= 0 ? question(m, id, name) : 0;
    if (fd >= 0) e = qlen < 0 ? SYS_EINVAL : sys_connect(fd, &a);
    for (int attempt = 0; !e && attempt < 3; ++attempt) {
        question(m, id, name);                         /* again: the buffer holds the last reply */
        if ((e = sys_write(fd, m, (size_t)qlen)) < 0) break;
        e = SYS_ETIMEDOUT;
        for (uint32_t start = sys_uptime(); sys_uptime() - start < 2000u;) {
            struct nn_sockinfo i;
            sys_sockinfo(fd, &i);
            if (!i.rx) {
                if (nn_interrupted()) { e = SYS_EINTR; goto out; }
                sys_sleep(20);
                continue;
            }
            int n = sys_read(fd, m, MSG);
            if (n < 0) { e = n; goto out; }
            if ((e = answer(m, n, id, ip)) <= 0) goto out;
            e = SYS_ETIMEDOUT;                     /* someone else's: keep waiting */
        }
    }
out:
    if (fd >= 0) sys_close(fd);
    sys_free(m);
    return e;
}

int cmd_host(int argc, char **argv)
{
    if (argc != 2) { printf("usage: host NAME\n"); return 2; }
    uint8_t ip[4];
    int e = dns_resolve(argv[1], ip);
    if (e == SYS_ENOENT) { printf("host: %s: not found\n", argv[1]); return 1; }
    if (e == SYS_EINVAL) { printf("host: no DNS server (ifconfig eth0 ... dns IP, or dhcp)\n"); return 1; }
    if (e) { printf("host: %s: %s\n", argv[1], errstr(e)); return 1; }
    printf("%s has address %u.%u.%u.%u\n", argv[1], ip[0], ip[1], ip[2], ip[3]);
    return 0;
}
