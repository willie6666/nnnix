/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Exercise the real supervisor: every confirmed CH376/rail loss must reach
 * usbfs, including a complete generation wrap without any filesystem use. */
#include "../../kernel/modules.h"
#include "../../arch/nuc140/arch.h"
#include "../../drivers/ch376.h"
#include "../../net/net.h"
#include "check.h"

struct w5500_netcfg net_cfg;
const uint8_t net_buffers_kib[8] = { 0 };
static int rail = 1, chip = 1, ethernet = 1, busy, losses;
static int ch_irq, spi;
void usbfs_media_lost(void) { ++losses; }
void kprintf(const char *fmt, ...) { (void)fmt; }
void msleep(uint32_t ms) { (void)ms; }
int board_modules_powered(void) { return rail; }
void board_spi_pins(int on) { spi = on; }
void board_w5500_reset(int on) { (void)on; }
void board_w5500_irq(int on) { (void)on; }
void board_ch376_irq(int on) { ch_irq = on; }
int ch376_trylock(void) { return !busy; }
void ch376_unlock(void) { }
int ch376_present(void) { return chip; }
uint8_t ch376_version(void) { return 0x43; }
int w5500_present(void) { return ethernet; }
uint8_t w5500_phy(void) { return 7; }
void w5500_configure(const struct w5500_netcfg *cfg, const uint8_t buffers[8])
{ (void)cfg; (void)buffers; }
unsigned net_prefix(const uint8_t mask[4]) { (void)mask; return 30; }

int main(void)
{
    modules_poll();
    CHECK_EQ_INT(modules.ch376, 1);
    CHECK_EQ_INT(losses, 0);
    CHECK(ch_irq && spi);
    rail = 0;                          /* two pending interrupts are not a rail loss */
    modules_poll();
    CHECK(modules.powered);
    CHECK_EQ_INT(losses, 0);
    rail = 1;
    for (int i = 0; i < 126; ++i) {
        chip = 0;
        modules_poll();
        CHECK_EQ_INT(modules.ch376, 0);
        CHECK_EQ_INT(losses, i + 1);
        modules_poll();                 /* repeated absence is one event */
        CHECK_EQ_INT(losses, i + 1);
        chip = 1;
        modules_poll();
    }
    CHECK_EQ_INT(modules.ch376, 1);      /* reused twice, with 126 distinct loss events */
    chip = ethernet = rail = 0;
    busy = 1;                          /* fsd owns the chip: defer the probe */
    modules_poll();
    CHECK(modules.powered && modules.ch376);
    CHECK_EQ_INT(losses, 126);
    busy = 0;
    modules_poll();                     /* confirmed whole-rail loss */
    CHECK_EQ_INT(losses, 127);
    CHECK(!modules.powered && !modules.ch376 && !ch_irq && !spi);
    modules_poll();
    CHECK_EQ_INT(losses, 127);
    chip = ethernet = rail = 1;
    modules_poll();
    CHECK(modules.powered && modules.ch376 && ch_irq && spi);
    CHECK_EQ_INT(losses, 127);
    return check_summary("test_modules");
}
