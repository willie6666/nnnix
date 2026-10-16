/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Bootloader decisions (boot/update.c) against modelled NOR flashes. */
#include "../../boot/update.h"
#include "../../lib/fwupdate.h"
#include "check.h"
#include <stdlib.h>
#include <string.h>

static uint8_t spi[2u * 1024u * 1024u];
static uint32_t aprom[AP_SIZE / 4];
static int writes, erases, fail_after = -1;
static char log_text[1024];

static void spi_read(uint32_t addr, void *buf, uint32_t n) { memcpy(buf, spi + addr, n); }
static int spi_program(uint32_t addr, const void *buf, uint32_t n)
{
    const uint8_t *p = buf;
    for (uint32_t i = 0; i < n; ++i) spi[addr + i] &= p[i];
    return 0;
}
static uint32_t ap_read(uint32_t addr) { return aprom[addr / 4]; }
static int ap_erase(uint32_t page) { memset((uint8_t *)aprom + page, 0xff, AP_PAGE); erases++; return 0; }
static int ap_write(uint32_t addr, uint32_t word)
{
    if (fail_after == 0) return -1;                   /* "power lost": nothing more is written */
    if (fail_after > 0) fail_after--;
    aprom[addr / 4] &= word;
    writes++;
    return 0;
}
static void log_line(const char *s) { strncat(log_text, s, sizeof log_text - strlen(log_text) - 1); }
static const struct boot_hw hw = { spi_read, spi_program, ap_read, ap_erase, ap_write, log_line };

static int boot(void) { writes = erases = 0; log_text[0] = 0; return boot_decide(&hw); }

static uint32_t crc32(const uint8_t *p, uint32_t n) { return ~boot_crc(~0u, p, n); }

/* A firmware image: vectors, then a pattern. */
static void make_image(uint8_t *img, uint32_t size, uint8_t seed)
{
    for (uint32_t i = 0; i < size; ++i) img[i] = (uint8_t)(i * 7u + seed);
    uint32_t v[2] = { 0x20004000u, 0x101u };
    memcpy(img, v, 8);
}

static void stage(const uint8_t *img, uint32_t size, uint32_t crc)
{
    memset(spi + FW_RECORD_ADDR, 0xff, 4096 + FW_IMAGE_MAX);
    memcpy(spi + FW_IMAGE_ADDR, img, size);
    struct fw_record r = { FW_MAGIC, size, crc, ~(FW_MAGIC ^ size ^ crc), FW_STAGED };
    memcpy(spi + FW_RECORD_ADDR, &r, sizeof r);
}

static struct fw_record record(void) { struct fw_record r; memcpy(&r, spi + FW_RECORD_ADDR, sizeof r); return r; }

int main(void)
{
    static uint8_t old[4096], img[6000];
    memset(spi, 0xff, sizeof spi);
    memset(aprom, 0xff, sizeof aprom);

    /* Empty APROM and no update: stay. */
    CHECK_EQ_INT(boot(), BOOT_STAY);
    CHECK(strstr(log_text, "no firmware") != NULL);

    /* A firmware already in APROM (put there with Nu-Link), no update: start it, write nothing. */
    make_image(old, sizeof old, 1);
    memcpy(aprom, old, sizeof old);
    CHECK_EQ_INT(boot(), BOOT_RUN_APROM);
    CHECK_EQ_INT(writes + erases, 0);

    /* A staged update is installed, verified and marked done. */
    make_image(img, sizeof img, 2);
    stage(img, sizeof img, crc32(img, sizeof img));
    CHECK_EQ_INT(boot(), BOOT_RUN_APROM);
    CHECK(!memcmp(aprom, img, sizeof img));
    CHECK_EQ_INT(record().state, FW_DONE);
    CHECK_EQ_INT(erases, (sizeof img + AP_PAGE - 1) / AP_PAGE);
    CHECK(strstr(log_text, "written and verified") != NULL);

    /* Done: the next boot leaves APROM alone, also after Nu-Link replaced it. */
    CHECK_EQ_INT(boot(), BOOT_RUN_APROM);
    CHECK_EQ_INT(writes + erases, 0);
    memcpy(aprom, old, sizeof old);
    CHECK_EQ_INT(boot(), BOOT_RUN_APROM);
    CHECK_EQ_INT(writes + erases, 0);
    CHECK(!memcmp(aprom, old, sizeof old));

    /* An interrupted installation is repeated at the next boot. */
    stage(img, sizeof img, crc32(img, sizeof img));
    fail_after = 700;
    CHECK_EQ_INT(boot(), BOOT_STAY);
    CHECK_EQ_INT(record().state, FW_STAGED);
    fail_after = -1;
    CHECK_EQ_INT(boot(), BOOT_RUN_APROM);
    CHECK(!memcmp(aprom, img, sizeof img));
    CHECK_EQ_INT(record().state, FW_DONE);

    /* APROM erased later: restored from the stored image. */
    memset(aprom, 0xff, sizeof aprom);
    CHECK_EQ_INT(boot(), BOOT_RUN_APROM);
    CHECK(strstr(log_text, "restoring") != NULL);
    CHECK(!memcmp(aprom, img, sizeof img));

    /* A damaged staged image, or a damaged record, is not installed. */
    memcpy(aprom, old, sizeof old);
    stage(img, sizeof img, crc32(img, sizeof img) ^ 1u);
    CHECK_EQ_INT(boot(), BOOT_RUN_APROM);
    CHECK_EQ_INT(writes + erases, 0);
    CHECK(strstr(log_text, "damaged") != NULL);
    stage(img, sizeof img, crc32(img, sizeof img));
    spi[FW_RECORD_ADDR + 4] ^= 0x10;                  /* size changed: check fails */
    CHECK_EQ_INT(boot(), BOOT_RUN_APROM);
    CHECK_EQ_INT(writes + erases, 0);
    CHECK(!memcmp(aprom, old, sizeof old));
    return check_summary("test_boot");
}
