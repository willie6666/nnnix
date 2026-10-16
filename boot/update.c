/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Bootloader logic: apply a staged update (lib/fwupdate.h), then decide
 * whether APROM can be started. */
#include "update.h"
#include "../lib/fwupdate.h"

uint32_t boot_crc(uint32_t crc, const uint8_t *p, uint32_t n)
{
    static const uint32_t table[16] = {
        0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac, 0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
        0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c, 0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c,
    };
    while (n--) {
        crc = (crc >> 4) ^ table[(crc ^ *p) & 0xfu];
        crc = (crc >> 4) ^ table[(crc ^ (*p++ >> 4)) & 0xfu];
    }
    return crc;
}

static uint32_t spi_image_crc(const struct boot_hw *hw, uint32_t size)
{
    uint8_t buf[64];
    uint32_t crc = ~0u;
    for (uint32_t off = 0; off < size; off += sizeof buf) {
        uint32_t n = size - off < sizeof buf ? size - off : sizeof buf;
        hw->spi_read(FW_IMAGE_ADDR + off, buf, n);
        crc = boot_crc(crc, buf, n);
    }
    return ~crc;
}

static uint32_t aprom_crc(const struct boot_hw *hw, uint32_t size)
{
    uint32_t crc = ~0u;
    for (uint32_t off = 0; off < size; off += 4) {
        uint32_t w = hw->ap_read(off);
        uint8_t b[4] = { (uint8_t)w, (uint8_t)(w >> 8), (uint8_t)(w >> 16), (uint8_t)(w >> 24) };
        crc = boot_crc(crc, b, 4);
    }
    return ~crc;
}

/* Erase and write APROM from the SPI image, then check it. 0 on success. */
static int program(const struct boot_hw *hw, const struct fw_record *r)
{
    uint32_t words[16];
    for (uint32_t page = 0; page < r->size; page += AP_PAGE)
        if (hw->ap_erase(page)) { hw->log("boot: APROM erase failed\n"); return -1; }
    for (uint32_t off = 0; off < r->size; off += sizeof words) {
        uint32_t n = r->size - off < sizeof words ? r->size - off : sizeof words;
        hw->spi_read(FW_IMAGE_ADDR + off, words, n);
        for (uint32_t i = 0; i < n / 4u; ++i)
            if (hw->ap_write(off + 4u * i, words[i])) { hw->log("boot: APROM write failed\n"); return -1; }
    }
    if (aprom_crc(hw, r->size) != r->crc) { hw->log("boot: APROM verify failed\n"); return -1; }
    return 0;
}

static int aprom_valid(const struct boot_hw *hw)
{
    return fw_vectors_valid(hw->ap_read(0), hw->ap_read(4), AP_SIZE);
}

int boot_decide(const struct boot_hw *hw)
{
    struct fw_record r;
    hw->spi_read(FW_RECORD_ADDR, &r, sizeof r);
    int have = fw_record_valid(&r) && (r.state == FW_STAGED || r.state == FW_DONE);
    if (have && spi_image_crc(hw, r.size) != r.crc) {
        hw->log("boot: stored image damaged, ignored\n");
        have = 0;
    }
    /* A staged image (also one whose programming was interrupted) is written
     * now, unless APROM already holds exactly it (only marking it done failed
     * or was cut short); a done one again only if APROM no longer holds a
     * firmware. */
    if (have && r.state == FW_STAGED && aprom_crc(hw, r.size) == r.crc) {
        hw->log("boot: update already in APROM\n");
    } else if (have && (r.state == FW_STAGED || !aprom_valid(hw))) {
        hw->log(r.state == FW_STAGED ? "boot: installing update\n" : "boot: APROM empty, restoring\n");
        if (program(hw, &r)) return BOOT_STAY;
        hw->log("boot: APROM written and verified\n");
    }
    if (have && r.state == FW_STAGED) {
        uint32_t addr = FW_RECORD_ADDR + (uint32_t)((uint8_t *)&r.state - (uint8_t *)&r), state = FW_DONE;
        int e = hw->spi_program(addr, &state, 4);
        hw->spi_read(addr, &state, 4);
        if (e || state != FW_DONE) hw->log("boot: could not mark the update installed\n");
    }
    if (!aprom_valid(hw)) { hw->log("boot: no firmware in APROM\n"); return BOOT_STAY; }
    return BOOT_RUN_APROM;
}
