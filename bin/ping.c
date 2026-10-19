/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Small ICMP Echo client. W5500 supplies the IPv4 header and source filtering;
 * we supply and validate the complete ICMP message (RFC 792). */
#include "../lib/dns.h"
#include "../user/cmds.h"
#include "../lib/stdio.h"
#include "../lib/string.h"
#include "../lib/sys.h"

#define PACKET 64

static uint16_t checksum(const uint8_t *p, unsigned n)
{
    uint32_t sum = 0;
    while (n > 1) { sum += (unsigned)p[0] << 8 | p[1]; p += 2; n -= 2; }
    if (n) sum += (unsigned)*p << 8;
    while (sum >> 16) sum = (sum & 0xffffu) + (sum >> 16);
    return (uint16_t)~sum;
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void print_ip(const uint8_t *p) { printf("%u.%u.%u.%u", p[0], p[1], p[2], p[3]); }

static unsigned number(const char *s, unsigned max)
{
    unsigned n = 0;
    if (!*s) return 0;
    for (; *s; ++s) {
        if (*s < '0' || *s > '9' || n > max / 10) return 0;
        n = n * 10 + (unsigned)(*s - '0');
        if (n > max) return 0;
    }
    return n;
}

int cmd_ping(int argc, char **argv)
{
    unsigned count = 0, timeout = 2000, i = 1;
    while (i < (unsigned)argc && argv[i][0] == '-') {
        int c = !strcmp(argv[i], "-c"), w = !strcmp(argv[i], "-W");
        if ((!c && !w) || ++i == (unsigned)argc) goto usage;
        unsigned n = number(argv[i++], c ? 65535u : 60u);
        if (!n) goto usage;
        if (c) count = n;
        else timeout = n * 1000u;
    }
    if (i + 1 != (unsigned)argc) goto usage;
    (void)nn_interrupted();
    struct nn_sockaddr a = { .port = 0 };
    int e = dns_resolve(argv[i], a.ip);
    if (e) {
        printf("ping: %s: %s\n", argv[i], e == SYS_ENOENT ? "no such host" :
               e == SYS_EINVAL ? "invalid name or no DNS server (ifconfig or dhcp)" : errstr(e));
        return e == SYS_EINTR ? 130 : 1;
    }
    int fd = sys_socket(NN_SOCK_ICMP);
    if (fd < 0) { printf("ping: %s\n", errstr(fd)); return 1; }
    e = sys_connect(fd, &a);
    int rc = 1;
    if (e) goto out;
    printf("PING %s (", argv[i]); print_ip(a.ip); printf("): %u data bytes\n", PACKET - 8);
    uint32_t sent = 0, received = 0;
    uint16_t id = (uint16_t)(sys_uptime() ^ (uint32_t)sys_getpid());
    while (!count || sent < count) {
        if (nn_interrupted()) { rc = 130; break; }
        uint8_t p[PACKET], reply[PACKET];
        memset(p, 0, sizeof p);
        p[0] = 8;
        put16(p + 4, id);
        put16(p + 6, (uint16_t)(sent + 1));
        uint32_t start = sys_uptime();
        memcpy(p + 8, &start, sizeof start);
        for (unsigned j = 12; j < sizeof p; ++j) p[j] = (uint8_t)j;
        put16(p + 2, checksum(p, sizeof p));
        int n = sys_write(fd, p, sizeof p);
        ++sent;
        if (n != sizeof p) { e = n < 0 ? n : SYS_EIO; break; }
        int got = 0;
        while (sys_uptime() - start < timeout) {
            if (nn_interrupted()) { rc = 130; break; }
            struct nn_sockinfo info;
            e = sys_sockinfo(fd, &info);
            if (e) break;
            if (!info.rx) { sys_sleep(10); continue; }
            n = sys_read(fd, reply, sizeof reply);
            if (n == SYS_EAGAIN) continue;
            if (n < 0) { e = n; break; }
            /* Truncated, corrupt, stale and other ICMP messages do not count. */
            if (n != sizeof reply || reply[0] || reply[1] || checksum(reply, sizeof reply) ||
                memcmp(reply + 4, p + 4, sizeof p - 4)) continue;
            ++received;
            got = 1;
            printf("%u bytes from ", (unsigned)n); print_ip(a.ip);
            printf(": icmp_seq=%u time=%u ms\n", (unsigned)(uint16_t)sent, sys_uptime() - start);
            break;
        }
        if (rc == 130 || e) break;
        if (!got) printf("Request timeout for icmp_seq=%u\n", (unsigned)(uint16_t)sent);
        if (count && sent == count) break;
        while (sys_uptime() - start < 1000u) {
            if (nn_interrupted()) { rc = 130; break; }
            sys_sleep(10);
        }
        if (rc == 130) break;
    }
    printf("%u packets transmitted, %u received, %u%% packet loss\n", sent, received,
           sent ? (sent - received) * 100u / sent : 0);
    if (rc != 130) rc = received && !e ? 0 : 1;
out:
    if (e) printf("ping: %s\n", errstr(e));
    sys_close(fd);
    return rc;
usage:
    printf("usage: ping [-c COUNT] [-W SECONDS] HOST\n");
    return 2;
}
