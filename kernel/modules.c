/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "modules.h"
#include "klog.h"
#include "diag.h"
#include "../arch/nuc140/arch.h"
#include "../drivers/ch376.h"
#include "../drivers/w5500.h"

struct modules_state modules;

/* M0 static network: the NanoPi test peer is 198.18.0.1/30. */
static const struct w5500_netcfg netcfg = {
    .mac = { 0x02, 0x37, 0x65, 0x55, 0x00, 0x01 },     /* locally administered */
    .ip = { 198, 18, 0, 2 }, .mask = { 255, 255, 255, 252 }, .gateway = { 0, 0, 0, 0 },
};
static const uint8_t buffers_kib[8] = { 16, 0, 0, 0, 0, 0, 0, 0 };

void modules_poll(void)
{
    int powered = board_modules_powered();
    if (powered != modules.powered) {
        if (powered) delay_ms(200);                    /* let the rail settle */
        powered = board_modules_powered();
        if (powered != modules.powered) {
            modules = (struct modules_state){ .powered = (uint8_t)powered };
            kprintf(powered ? "modules: power on\n" : "modules: power off, SPI pins released\n");
            board_spi_pins(powered);
        }
    }
    if (!modules.powered) return;

    int ch = ch376_present();
    if (ch != modules.ch376) {
        modules.ch376 = (uint8_t)ch;
        if (ch) kprintf("ch376: present, version 0x%02x\n", ch376_version());
        else kprintf("ch376: not responding\n");
    }
    int w = w5500_present();
    if (w != modules.w5500) {
        modules.w5500 = (uint8_t)w;
        if (w) {
            w5500_configure(&netcfg, buffers_kib);
            kprintf("w5500: present, ip %u.%u.%u.%u/30\n", netcfg.ip[0], netcfg.ip[1], netcfg.ip[2], netcfg.ip[3]);
            diag_start();
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
