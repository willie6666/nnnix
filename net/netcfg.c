/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Interface addresses and network services. The board boots with a static
 * address so that it is reachable before /etc/rc runs; rc (or a user) may then
 * set another one, ask DHCP for one, or stop services. */
#include "net.h"
#include "../arch/nuc140/arch.h"
#include "../kernel/klog.h"
#include "../kernel/modules.h"
#include "../lib/string.h"

/* The NanoPi test peer is 198.18.0.1/30. */
struct w5500_netcfg net_cfg = {
    .mac = { 0x02, 0x37, 0x65, 0x55, 0x00, 0x01 },     /* locally administered */
    .ip = { 198, 18, 0, 2 }, .mask = { 255, 255, 255, 252 }, .gateway = { 0, 0, 0, 0 },
};
uint8_t net_dns[4];
volatile uint8_t net_services = NN_SVC_TELNETD | NN_SVC_HTTPD;
/* 16 KiB per direction in total. */
const uint8_t net_buffers_kib[8] = { 4, 2, 2, 2, 2, 2, 1, 1 };

unsigned net_prefix(const uint8_t mask[4])
{
    uint32_t m = (uint32_t)mask[0] << 24 | (uint32_t)mask[1] << 16 | (uint32_t)mask[2] << 8 | mask[3];
    unsigned n = 0;
    while (n < 32 && (m & (0x80000000u >> n))) ++n;
    return n < 32 && (m << n) ? 33u : n;
}

void net_apply(void)
{
    if (modules.w5500) w5500_set_addr(&net_cfg);
}

static void log_addr(const char *who)
{
    const uint8_t *ip = net_cfg.ip, *gw = net_cfg.gateway;
    kprintf("%s: %u.%u.%u.%u/%u gw %u.%u.%u.%u\n", who, ip[0], ip[1], ip[2], ip[3], net_prefix(net_cfg.mask),
            gw[0], gw[1], gw[2], gw[3]);
}

int net_config(struct nn_netcfg *c, int op)
{
    switch (op) {
    case NN_NET_GET:
        memset(c, 0, sizeof *c);
        memcpy(c->mac, net_cfg.mac, 6);
        memcpy(c->ip, net_cfg.ip, 4);
        memcpy(c->mask, net_cfg.mask, 4);
        memcpy(c->gw, net_cfg.gateway, 4);
        memcpy(c->dns, net_dns, 4);
        c->services = net_services;
        c->link = modules.link;
        dhcp_status(c);
        return 0;
    case NN_NET_SET:
        if (net_prefix(c->mask) > 32) return SYS_EINVAL;
        dhcp_stop();
        uint32_t s = irq_save();
        memcpy(net_cfg.ip, c->ip, 4);
        memcpy(net_cfg.mask, c->mask, 4);
        memcpy(net_cfg.gateway, c->gw, 4);
        memcpy(net_dns, c->dns, 4);
        irq_restore(s);
        net_apply();
        log_addr("net");
        return 0;
    case NN_NET_DHCP:
        if (!modules.w5500) return SYS_ENODEV;
        dhcp_start();
        return 0;
    case NN_NET_SERVICES:
        if (c->services & ~(NN_SVC_TELNETD | NN_SVC_HTTPD)) return SYS_EINVAL;
        if (c->services != net_services)
            kprintf("net: telnetd %s, httpd %s\n", (c->services & NN_SVC_TELNETD) ? "on" : "off",
                    (c->services & NN_SVC_HTTPD) ? "on" : "off");
        net_services = c->services;
        return 0;
    default:
        return SYS_EINVAL;
    }
}

/* Used by the DHCP client when a lease arrives. */
void net_set_lease(const uint8_t ip[4], const uint8_t mask[4], const uint8_t gw[4], const uint8_t dns[4])
{
    uint32_t s = irq_save();
    memcpy(net_cfg.ip, ip, 4);
    memcpy(net_cfg.mask, mask, 4);
    memcpy(net_cfg.gateway, gw, 4);
    memcpy(net_dns, dns, 4);
    irq_restore(s);
    net_apply();
    log_addr("dhcp");
}
