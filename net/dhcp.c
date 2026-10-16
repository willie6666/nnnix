/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DHCP client (RFC 2131) on W5500 socket 7, run by the kernel daemon.
 *
 * DISCOVER -> OFFER -> REQUEST -> ACK, sent 3 times (2, 4, 8 s apart); renewal at half
 * the lease (at most daily), retried every minute until the lease ends, then a
 * new DISCOVER. Packets are written to and parsed from the chip buffers in small
 * pieces, so the client needs no packet buffer. Requests ask for broadcast
 * replies: the chip only accepts unicast frames for its current address.
 * While a request runs and when a renewal fails, the old address stays. */
#include "net.h"
#include "../arch/nuc140/arch.h"
#include "../kernel/kd.h"
#include "../kernel/klog.h"
#include "../kernel/modules.h"
#include "../lib/string.h"

#define SN NET_SOCK_DHCP
#define TRIES 3
#define MAX_LEASE (20u * 86400u)             /* jiffies comparisons stay within 2^31 ms */

enum { DISCOVER = 1, OFFER = 2, REQUEST = 3, ACK = 5, NAK = 6 };

static struct {
    volatile uint8_t state, start;
    uint8_t tries, open;
    uint32_t xid, next, bound_at, lease;
    uint8_t server[4], offered[4];
} d;

struct reply { uint8_t type, yiaddr[4], server[4], mask[4], router[4], dns[4]; uint32_t lease; };

static int due(uint32_t at) { return (int32_t)(jiffies - at) >= 0; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static void put(const void *p, uint16_t n) { (void)w5500_write(SN, p, n); }
static void put_zero(uint16_t n)
{
    static const uint8_t zero[16];
    while (n) {
        uint16_t k = n < sizeof zero ? n : (uint16_t)sizeof zero;
        put(zero, k);
        n = (uint16_t)(n - k);
    }
}

static void transmit(void)
{
    static const uint8_t bcast[4] = { 255, 255, 255, 255 };
    uint8_t type = d.state == NN_DHCP_SELECTING ? DISCOVER : REQUEST;
    uint8_t b[48];
    memset(b, 0, 28);
    b[0] = 1; b[1] = 1; b[2] = 6;                      /* BOOTREQUEST, Ethernet, 6-byte address */
    for (int i = 0; i < 4; ++i) b[4 + i] = (uint8_t)(d.xid >> (24 - 8 * i));
    b[10] = 0x80;                                      /* broadcast replies */
    if (d.state == NN_DHCP_RENEWING) memcpy(b + 12, net_cfg.ip, 4);    /* ciaddr */
    w5500_set_peer(SN, bcast, 67);
    put(b, 28);
    put(net_cfg.mac, 6);
    put_zero(10 + 64 + 128);                           /* chaddr padding, sname, file */
    uint16_t n = 0;
    static const uint8_t head[] = { 99, 130, 83, 99, 53, 1 };
    memcpy(b, head, sizeof head);
    n = sizeof head;
    b[n++] = type;
    b[n++] = 61; b[n++] = 7; b[n++] = 1;               /* client id: hardware address */
    memcpy(b + n, net_cfg.mac, 6); n += 6;
    if (d.state == NN_DHCP_REQUESTING) {
        b[n++] = 50; b[n++] = 4; memcpy(b + n, d.offered, 4); n += 4;
        b[n++] = 54; b[n++] = 4; memcpy(b + n, d.server, 4); n += 4;
    }
    static const uint8_t tail[] = { 55, 4, 1, 3, 6, 51, 12, 5, 'n', 'n', 'n', 'i', 'x', 255 };
    memcpy(b + n, tail, sizeof tail);
    n += sizeof tail;
    put(b, n);
    if (236u + n < 300u) put_zero((uint16_t)(300u - 236u - n));     /* BOOTP minimum size */
    (void)w5500_push(SN);
    d.next = jiffies + (2000u << (d.tries < 2 ? d.tries : 2));
    d.tries++;
}

/* ---- replies, read from the chip in pieces ---- */
static uint16_t left;                /* bytes of the current datagram not read yet */

static int get(void *p, uint16_t n)
{
    if (n > left || w5500_recv(SN, p, n) != n) return -1;
    left = (uint16_t)(left - n);
    return 0;
}
static void skip(uint16_t n)
{
    if (n > left) n = left;
    w5500_skip(SN, n);
    left = (uint16_t)(left - n);
}

static int parse(struct reply *r)
{
    uint8_t b[8];
    if (get(b, 8) || b[0] != 2 || be32(b + 4) != d.xid) return -1;   /* op htype hlen hops xid */
    skip(8);                                            /* secs flags ciaddr */
    if (get(r->yiaddr, 4)) return -1;
    skip(8);                                            /* siaddr giaddr */
    if (get(b, 6) || memcmp(b, net_cfg.mac, 6)) return -1;
    skip(10 + 192);                                     /* chaddr padding, sname, file */
    if (get(b, 4) || be32(b) != 0x63825363u) return -1;
    for (;;) {
        uint8_t code, len, v[4] = { 0 };
        if (get(&code, 1) || code == 255) break;
        if (!code) continue;
        if (get(&len, 1)) return -1;
        uint8_t k = len < 4 ? len : 4;
        if (get(v, k)) return -1;
        skip((uint16_t)(len - k));
        switch (code) {
        case 53: r->type = v[0]; break;
        case 54: memcpy(r->server, v, 4); break;
        case 1: memcpy(r->mask, v, 4); break;
        case 3: memcpy(r->router, v, 4); break;
        case 6: memcpy(r->dns, v, 4); break;
        case 51: r->lease = be32(v); break;
        }
    }
    return r->type ? 0 : -1;
}

static void close_socket(void)
{
    if (d.open) w5500_close(SN);
    d.open = 0;
}

static void bind_lease(const struct reply *r)
{
    d.lease = r->lease ? (r->lease < MAX_LEASE ? r->lease : MAX_LEASE) : 3600u;
    d.bound_at = jiffies;
    uint32_t renew = d.lease / 2u < 86400u ? d.lease / 2u : 86400u;
    d.next = jiffies + renew * 1000u;
    d.state = NN_DHCP_BOUND;
    close_socket();
    if (r->server[0]) memcpy(d.server, r->server, 4);
    static const uint8_t class_c[4] = { 255, 255, 255, 0 };
    net_set_lease(r->yiaddr, r->mask[0] ? r->mask : class_c, r->router, r->dns);
    kprintf("dhcp: lease %u s from %u.%u.%u.%u\n", d.lease, d.server[0], d.server[1], d.server[2], d.server[3]);
}

static void handle(const struct reply *r)
{
    if (d.state == NN_DHCP_SELECTING && r->type == OFFER) {
        memcpy(d.offered, r->yiaddr, 4);
        memcpy(d.server, r->server, 4);
        d.state = NN_DHCP_REQUESTING;
        d.tries = 0;
        transmit();
    } else if (d.state == NN_DHCP_REQUESTING || d.state == NN_DHCP_RENEWING) {
        if (r->type == ACK) bind_lease(r);
        else if (r->type == NAK) {
            kprintf("dhcp: refused, starting over\n");
            d.state = NN_DHCP_SELECTING;
            d.tries = 0;
            transmit();
        }
    }
}

static void receive(void)
{
    while (d.open && w5500_rx_available(SN) >= 8) {
        uint8_t h[8];
        w5500_recv(SN, h, 8);                          /* sender address, port, length */
        left = (uint16_t)(h[6] << 8 | h[7]);
        static struct reply r;                         /* kd's stack is small */
        memset(&r, 0, sizeof r);
        int ok = !parse(&r);
        skip(left);
        if (ok) handle(&r);
    }
}

static void begin(uint8_t state)
{
    if (w5500_open(SN, W5500_MODE_UDP, 68)) {
        kprintf("dhcp: cannot open socket\n");
        d.state = NN_DHCP_FAILED;
        return;
    }
    d.open = 1;
    d.xid = d.xid * 1103515245u + 12345u + jiffies + net_cfg.mac[5];
    d.state = state;
    d.tries = 0;
    transmit();
}

void dhcp_start(void)
{
    d.state = NN_DHCP_SELECTING;                       /* shown at once; kd sends */
    d.start = 1;
    kd_notify();
}

void dhcp_stop(void) { d.start = 0; d.state = NN_DHCP_OFF; }

void dhcp_status(struct nn_netcfg *c)
{
    c->dhcp = d.state;
    memcpy(c->server, d.server, 4);
    if (d.state == NN_DHCP_BOUND || d.state == NN_DHCP_RENEWING) {
        uint32_t used = (jiffies - d.bound_at) / 1000u;
        c->lease = used < d.lease ? d.lease - used : 0;
    }
}

void dhcp_poll(void)
{
    if (d.start) {
        d.start = 0;
        begin(NN_DHCP_SELECTING);
        return;
    }
    switch (d.state) {
    case NN_DHCP_SELECTING:
    case NN_DHCP_REQUESTING:
    case NN_DHCP_RENEWING:
        receive();
        if (d.state == NN_DHCP_BOUND || !due(d.next)) break;
        if (d.tries < TRIES) { transmit(); break; }
        close_socket();
        if (d.state == NN_DHCP_RENEWING) {             /* keep the address, try again later */
            if (due(d.bound_at + d.lease * 1000u)) {
                kprintf("dhcp: lease expired, keeping the address\n");
                begin(NN_DHCP_SELECTING);
            } else {
                d.state = NN_DHCP_BOUND;
                d.next = jiffies + 60000u;
            }
        } else {
            kprintf("dhcp: no answer\n");
            d.state = NN_DHCP_FAILED;
        }
        break;
    case NN_DHCP_BOUND:
        if (due(d.next)) begin(NN_DHCP_RENEWING);
        break;
    default:                                           /* stopped: drop a half-done exchange */
        close_socket();
        break;
    }
}
