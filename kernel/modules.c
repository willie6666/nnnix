/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "modules.h"
#include "klog.h"
#include "task.h"
#include "../arch/nuc140/arch.h"
#include "../drivers/ch376.h"
#include "../drivers/w5500.h"
#include "../net/net.h"

struct modules_state modules;

void modules_poll(void)
{
    int powered = board_modules_powered();
    if (powered != modules.powered) {
        if (powered) msleep(200);                      /* let the rail settle */
        powered = board_modules_powered();
        if (powered != modules.powered) {
            board_w5500_irq(0);
            board_ch376_irq(0);
            modules = (struct modules_state){ .powered = (uint8_t)powered };
            kprintf(powered ? "modules: power on\n" : "modules: power off, SPI pins released\n");
            board_spi_pins(powered);
            if (powered) {                             /* W5500 hardware reset */
                board_w5500_reset(1);
                msleep(2);
                board_w5500_reset(0);
                msleep(60);
            }
        }
    }
    if (!modules.powered) return;

    if (!ch376_trylock()) return;    /* a file operation is in progress: it is present */
    int ch = ch376_present();
    uint8_t version = ch ? ch376_version() : 0;
    ch376_unlock();
    if (ch != modules.ch376) {
        modules.ch376 = (uint8_t)ch;
        board_ch376_irq(ch);
        if (ch) kprintf("ch376: present, version 0x%02x\n", version);
        else kprintf("ch376: not responding\n");
    }
    int w = w5500_present();
    if (w != modules.w5500) {
        modules.w5500 = (uint8_t)w;
        if (w) {
            w5500_configure(&net_cfg, net_buffers_kib);    /* sockets closed: servers re-listen */
            board_w5500_irq(1);
            kprintf("w5500: present, ip %u.%u.%u.%u/%u\n", net_cfg.ip[0], net_cfg.ip[1], net_cfg.ip[2], net_cfg.ip[3],
                    net_prefix(net_cfg.mask));
        } else kprintf("w5500: not responding\n");
    }
    uint8_t link = w ? (uint8_t)((w5500_phy() & 7u) | 8u) : 0;
    if (link != modules.link) {
        modules.link = link;
        if (w) kprintf("w5500: link %s%s\n", (link & 1u) ? "up " : "down",
                       (link & 1u) ? ((link & 2u) ? ((link & 4u) ? "100M full" : "100M half")
                                                  : ((link & 4u) ? "10M full" : "10M half")) : "");
    }
}
