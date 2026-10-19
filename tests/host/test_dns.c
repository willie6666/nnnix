/* SPDX-License-Identifier: GPL-3.0-or-later */
/* lib/dns.c against a scripted server and a clock of its own (review R59):
 * three queries 2 s apart when nothing answers, an answer to the second
 * query after the first was lost, a lost datagram (EIO) not ending the
 * wait, NXDOMAIN at once. */
#include <stdint.h>
#include <string.h>
#include "../../lib/dns.h"
#include "../../lib/sys.h"
#include "check.h"

static uint32_t now, queries;
static int answer_query = -1, eio_once, nxdomain;   /* which query gets an answer (1 = the first) */
static uint8_t reply[512], query[512];
static int reply_len;
static uint8_t heap[512];

static void make_reply(int qlen)
{
    memcpy(reply, query, (size_t)qlen);
    reply[2] = 0x81;
    reply[3] = nxdomain ? 0x83 : 0x80;
    reply[7] = nxdomain ? 0 : 1;                       /* one answer */
    int n = qlen;
    if (!nxdomain) {
        static const uint8_t a[] = { 0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 93, 184, 216, 34 };
        memcpy(reply + n, a, sizeof a);
        n += (int)sizeof a;
    }
    reply_len = n;
}

intptr_t host_syscall(int n, intptr_t a, intptr_t b, intptr_t c)
{
    switch (n) {
    case SYS_NETCFG: {
        struct nn_netcfg *cfg = (struct nn_netcfg *)a;
        memset(cfg, 0, sizeof *cfg);
        cfg->dns[0] = 198; cfg->dns[1] = 18; cfg->dns[3] = 1;
        return 0;
    }
    case SYS_ALLOC: return (intptr_t)heap;
    case SYS_FREE: case SYS_CLOSE: case SYS_CONNECT: return 0;
    case SYS_SOCKET: return 3;
    case SYS_UPTIME: return (intptr_t)now;
    case SYS_SLEEP: now += (uint32_t)a; return 0;
    case SYS_WRITE:
        memcpy(query, (const void *)b, (size_t)c);
        if ((int)++queries == answer_query) make_reply((int)c);
        return c;
    case SYS_SOCKINFO: {
        struct nn_sockinfo *i = (struct nn_sockinfo *)b;
        memset(i, 0, sizeof *i);
        i->rx = (uint16_t)(reply_len || eio_once ? 1 : 0);
        return 0;
    }
    case SYS_READ:
        if (eio_once) { eio_once = 0; return SYS_EIO; }
        if (!reply_len) return SYS_EAGAIN;
        memcpy((void *)b, reply, (size_t)reply_len);
        { int r = reply_len; reply_len = 0; return r; }
    default:
        return SYS_ENOSYS;
    }
}

int nn_interrupted(void) { return 0; }
const char *errstr(int e) { (void)e; return "error"; }

static int resolve(int answer, int eio, int nx, uint8_t ip[4])
{
    now = queries = 0;
    reply_len = 0;
    answer_query = answer;
    eio_once = eio;
    nxdomain = nx;
    return dns_resolve("example.com", ip);
}

int main(void)
{
    uint8_t ip[4] = { 0 };
    CHECK_EQ_INT(resolve(-1, 0, 0, ip), SYS_ETIMEDOUT);
    CHECK_EQ_INT(queries, 3);
    CHECK(now >= 6000 && now < 6200);
    CHECK_EQ_INT(resolve(2, 0, 0, ip), 0);
    CHECK_EQ_INT(queries, 2);
    CHECK(ip[0] == 93 && ip[1] == 184 && ip[2] == 216 && ip[3] == 34);
    CHECK_EQ_INT(resolve(1, 1, 0, ip), 0);                      /* a lost datagram first */
    CHECK_EQ_INT(queries, 1);
    CHECK_EQ_INT(resolve(1, 0, 1, ip), SYS_ENOENT);
    CHECK_EQ_INT(queries, 1);
    CHECK(now < 100);
    return check_summary("test_dns");
}
