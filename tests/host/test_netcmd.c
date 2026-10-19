/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Commands against scripted ICMP, DNS and NTP peers, with a virtual clock. */
#include <stdint.h>
#include <string.h>
#include "../../lib/sys.h"
#include "check.h"

int cmd_ping(int argc, char **argv);
int cmd_ntpdate(int argc, char **argv);

enum { OK, TIMEOUT, FORGED, NO_DNS, NXDOMAIN, INTERRUPT, DNS_INTERRUPT, WRITE_ERROR, SHORT_WRITE };
static int mode, proto[8], service[8], sockets, closes, allocs, frees, queries, writes, at, total;
static uint8_t heap[512], packets[8][512];
static int lengths[8];
static uint32_t now, clock_set;

static uint16_t sum(const uint8_t *p, unsigned n)
{
    unsigned s = 0;
    for (unsigned i = 0; i < n; ++i) s += (unsigned)p[i] << (i & 1 ? 0 : 8);
    while (s >> 16) s = (s & 65535) + (s >> 16);
    return (uint16_t)~s;
}
static void sign_packet(uint8_t *p, unsigned n)
{
    p[2] = p[3] = 0;
    unsigned s = sum(p, n);
    p[2] = (uint8_t)(s >> 8); p[3] = (uint8_t)s;
}

intptr_t host_syscall(int n, intptr_t a, intptr_t b, intptr_t c)
{
    switch (n) {
    case SYS_UPTIME: return now;
    case SYS_TIME: return 1700000000;
    case SYS_STIME: clock_set = (uint32_t)a; return 0;
    case SYS_GETPID: return 42;
    case SYS_SLEEP: now += (uint32_t)a; return 0;
    case SYS_TTYINTR: return (mode == INTERRUPT || mode == DNS_INTERRUPT) && now >= 50;
    case SYS_NETCFG: {
        struct nn_netcfg *cfg = (struct nn_netcfg *)a;
        memset(cfg, 0, sizeof *cfg);
        if (mode != NO_DNS) memcpy(cfg->dns, "\xc6\x12\0\1", 4);
        return 0;
    }
    case SYS_ALLOC: allocs++; return (intptr_t)heap;
    case SYS_FREE: frees++; return 0;
    case SYS_OPEN: return SYS_ENOENT;  /* UTC, no /etc/tz */
    case SYS_SOCKET: proto[3 + sockets] = (int)a; return 3 + sockets++;
    case SYS_CONNECT: {
        const struct nn_sockaddr *peer = (const struct nn_sockaddr *)b;
        CHECK(!memcmp(peer->ip, "\xc6\x12\0\1", 4));
        service[a] = peer->port;
        if (proto[a] == NN_SOCK_ICMP) CHECK_EQ_INT(peer->port, 0);
        return 0;
    }
    case SYS_CLOSE: closes++; return 0;
    case SYS_SOCKINFO: {
        struct nn_sockinfo *info = (struct nn_sockinfo *)b;
        memset(info, 0, sizeof *info);
        info->rx = at < total ? 1 : 0;
        return 0;
    }
    case SYS_READ:
        if (at == total) return SYS_EAGAIN;
        CHECK(lengths[at] <= c);
        memcpy((void *)b, packets[at], (size_t)lengths[at]);
        return lengths[at++];
    case SYS_WRITE: {
        const uint8_t *p = (const uint8_t *)b;
        at = total = 0;
        if (service[a] == 53) {
            queries++;
            if (mode == DNS_INTERRUPT) return c;
            memcpy(packets[0], p, (size_t)c);
            packets[0][2] = 0x81; packets[0][3] = mode == NXDOMAIN ? 0x83 : 0x80;
            packets[0][7] = mode == NXDOMAIN ? 0 : 1;
            static const uint8_t answer[] = { 0xc0, 12, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 198, 18, 0, 1 };
            if (mode != NXDOMAIN) memcpy(packets[0] + c, answer, sizeof answer);
            lengths[0] = (int)c + (mode == NXDOMAIN ? 0 : (int)sizeof answer);
            total = 1;
            return c;
        }
        writes++;
        if (mode == WRITE_ERROR) return SYS_EIO;
        if (mode == SHORT_WRITE) return c - 1;
        if (mode == TIMEOUT || mode == INTERRUPT) return c;
        if (proto[a] == NN_SOCK_ICMP) {
            CHECK_EQ_INT(c, 64); CHECK_EQ_INT(p[0], 8); CHECK_EQ_INT(sum(p, (unsigned)c), 0);
            total = mode == FORGED ? 7 : 1;
            for (int j = 0; j < total; ++j) {
                memcpy(packets[j], p, (size_t)c); packets[j][0] = 0;
                if (j == 0 && total > 1) packets[j][0] = 8; /* request, not reply */
                if (j == 1) packets[j][4] ^= 1;             /* different id */
                if (j == 2) packets[j][7] ^= 1;             /* stale sequence */
                if (j == 3) packets[j][12] ^= 1;            /* altered payload */
                sign_packet(packets[j], (unsigned)c);
                if (j == 4) packets[j][2] ^= 1;             /* bad checksum */
                lengths[j] = (int)c - (j == 5 ? 1 : 0);    /* truncated */
            }
        } else {
            CHECK_EQ_INT(service[a], 123); CHECK_EQ_INT(c, 48);
            CHECK_EQ_INT(p[0], 0x23);
            total = mode == FORGED ? 2 : 1;
            for (int j = 0; j < total; ++j) {
                memset(packets[j], 0, 48);
                packets[j][0] = 0x24; packets[j][1] = 2;
                memcpy(packets[j] + 24, p + 40, 8);
                if (total > 1 && !j) packets[j][24] ^= 1;
                uint32_t ntp = 1893553445u + 2208988800u;
                for (int k = 0; k < 4; ++k) packets[j][40 + k] = (uint8_t)(ntp >> (24 - 8 * k));
                lengths[j] = 48;
            }
        }
        return c;
    }
    default: return SYS_ENOSYS;
    }
}
int nn_interrupted(void) { return (int)host_syscall(SYS_TTYINTR, 0, 0, 0); }
const char *errstr(int e) { (void)e; return "error"; }

static int invoke(int ntp, int m, int argc, char **argv)
{
    mode = m;
    sockets = closes = allocs = frees = queries = writes = at = total = 0;
    now = clock_set = 0;
    int r = ntp ? cmd_ntpdate(argc, argv) : cmd_ping(argc, argv);
    CHECK_EQ_INT(sockets, closes);
    CHECK_EQ_INT(allocs, frees);
    return r;
}

int main(void)
{
    char *ip[] = { "ping", "-c", "2", "-W", "1", "198.18.0.1" };
    CHECK_EQ_INT(invoke(0, OK, 6, ip), 0);
    CHECK_EQ_INT(writes, 2); CHECK_EQ_INT(queries, 0); CHECK(now >= 1000);
    char *domain[] = { "ping", "-c", "1", "web.test" };
    CHECK_EQ_INT(invoke(0, FORGED, 4, domain), 0);
    CHECK_EQ_INT(queries, 1); CHECK_EQ_INT(writes, 1); CHECK_EQ_INT(at, 7);
    CHECK_EQ_INT(invoke(0, TIMEOUT, 6, ip), 1);
    CHECK_EQ_INT(writes, 2); CHECK(now >= 2000 && now <= 2020);
    CHECK_EQ_INT(invoke(0, INTERRUPT, 6, ip), 130); CHECK(now < 100);
    CHECK_EQ_INT(invoke(0, WRITE_ERROR, 6, ip), 1);
    CHECK_EQ_INT(invoke(0, SHORT_WRITE, 6, ip), 1);
    CHECK_EQ_INT(invoke(0, NO_DNS, 4, domain), 1); CHECK_EQ_INT(sockets, 0);
    CHECK_EQ_INT(invoke(0, NXDOMAIN, 4, domain), 1); CHECK_EQ_INT(writes, 0);
    CHECK_EQ_INT(invoke(0, DNS_INTERRUPT, 4, domain), 130); CHECK(now < 100);
    const char *bad[] = { "0", "-1", "65536", "999999999999999", "1x", "" };
    for (unsigned j = 0; j < sizeof bad / sizeof *bad; ++j) {
        char *args[] = { "ping", "-c", (char *)bad[j], "198.18.0.1" };
        CHECK_EQ_INT(invoke(0, OK, 4, args), 2); CHECK_EQ_INT(sockets, 0);
    }
    char *ntp[] = { "ntpdate", "web.test" };
    CHECK_EQ_INT(invoke(1, FORGED, 2, ntp), 0);
    CHECK_EQ_INT(clock_set, 1893553445u); CHECK_EQ_INT(queries, 1); CHECK_EQ_INT(writes, 1);
    CHECK_EQ_INT(invoke(1, NO_DNS, 2, ntp), 1); CHECK_EQ_INT(sockets, 0);
    CHECK_EQ_INT(invoke(1, NXDOMAIN, 2, ntp), 1); CHECK_EQ_INT(clock_set, 0);
    CHECK_EQ_INT(invoke(1, DNS_INTERRUPT, 2, ntp), 130); CHECK_EQ_INT(clock_set, 0);
    ntp[1] = "198.18.0.1";
    CHECK_EQ_INT(invoke(1, OK, 2, ntp), 0); CHECK_EQ_INT(queries, 0);
    CHECK_EQ_INT(invoke(1, TIMEOUT, 2, ntp), 1); CHECK_EQ_INT(writes, 3); CHECK_EQ_INT(clock_set, 0);
    return check_summary("test_netcmd");
}
