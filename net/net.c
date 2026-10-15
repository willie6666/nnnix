/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "net.h"
#include "../kernel/klog.h"
#include "../kernel/task.h"
#include "../arch/nuc140/arch.h"
#include "../drivers/w5500.h"

int net_recv(int sn, void *buf, uint16_t n, uint32_t timeout_ms)
{
    uint32_t start = jiffies;
    for (;;) {
        uint16_t got = w5500_recv(sn, buf, n);
        if (got) return got;
        if (w5500_status(sn) != W5500_SOCK_ESTABLISHED) return 0;   /* peer closed, or reset */
        if (timeout_ms && jiffies - start >= timeout_ms) return -ETIMEDOUT;
        w5500_wait(sn, 100);
    }
}

int net_send(int sn, const void *buf, uint32_t n)
{
    const uint8_t *p = buf;
    uint32_t left = n, last = jiffies;
    while (left) {
        uint16_t room = w5500_tx_room(sn);
        if (!room) {
            /* Space comes back as the peer acknowledges; there is no event for that. */
            if (w5500_status(sn) != W5500_SOCK_ESTABLISHED || jiffies - last > 10000u) return -1;
            w5500_wait(sn, 5);
            continue;
        }
        uint16_t chunk = left < room ? (uint16_t)left : room;
        if (w5500_write(sn, p, chunk) || w5500_push(sn)) return -1;
        p += chunk;
        left -= chunk;
        last = jiffies;
    }
    return (int)n;
}

void net_close(int sn)
{
    w5500_flush(sn, 1000);
    w5500_disconnect(sn, 1000);
}

void net_peer(int sn, uint8_t ip[4], uint16_t *port) { w5500_peer(sn, ip, port); }

static void relisten(struct listener *l)
{
    if (w5500_tcp_listen(l->sn, l->port) == 0) {
        if (!l->announced) kprintf("%s: listening on tcp port %u\n", l->name, l->port);
        l->announced = 1;
    } else {
        kprintf("%s: listen failed on socket %u\n", l->name, l->sn);
    }
}

int listener_poll(struct listener *l)
{
    uint8_t sr = w5500_status(l->sn);
    if (sr != l->last_sr) { l->last_sr = sr; l->since = jiffies; }
    switch (sr) {
    case W5500_SOCK_ESTABLISHED:
        return 1;
    case W5500_SOCK_SYNRECV:
        /* A SYN that arrived while the peer's link was still coming up can sit
         * in SYN_RECV for ~32 s (measured after reboots). */
        if (jiffies - l->since > 2000u) {
            kprintf("%s: dropping stale half-open connection\n", l->name);
            relisten(l);
        }
        return 0;
    case W5500_SOCK_CLOSE_WAIT:
        w5500_disconnect(l->sn, 500);
        relisten(l);
        return 0;
    case W5500_SOCK_CLOSED:
        relisten(l);
        return 0;
    default:
        return 0;
    }
}
