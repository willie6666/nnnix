/* SPDX-License-Identifier: GPL-3.0-or-later */
/* kernel/xip.c on a simulated APROM: slots are written, found again after a
 * "reboot" (xip_init), give way oldest first, never while in use, a slot
 * whose writing was not finished is not one, and one whose text changed is
 * dropped. */
#include <stdint.h>
#include <string.h>
#include "../../kernel/xip.h"
#include "check.h"

#define AP_SIZE (128u * 1024u)
#define PAGE 512u

uint8_t host_aprom[AP_SIZE];
uint32_t host_flash_end;
static int erases, writes, fail_writes_after = -1;

int fmc_erase(uint32_t page)
{
    if (page % PAGE || page < host_flash_end || page >= AP_SIZE) return -1;
    memset(host_aprom + page, 0xff, PAGE);
    erases++;
    return 0;
}

int fmc_write(uint32_t addr, uint32_t word)
{
    if (addr % 4 || addr < host_flash_end || addr >= AP_SIZE) return -1;
    if (fail_writes_after >= 0 && writes >= fail_writes_after) return -1;
    uint32_t w;
    memcpy(&w, host_aprom + addr, 4);
    w &= word;                                   /* a write can only clear bits */
    memcpy(host_aprom + addr, &w, 4);
    writes++;
    return 0;
}

void kfree(void *p) { (void)p; }

uint32_t lfs_crc(uint32_t crc, const void *buffer, size_t size)   /* fs/lfs_port.c's, bit by bit */
{
    const uint8_t *p = buffer;
    for (size_t i = 0; i < size; ++i) {
        crc ^= p[i];
        for (int b = 0; b < 8; ++b) crc = crc >> 1 ^ (0xedb88320u & -(crc & 1u));
    }
    return crc;
}

static uint32_t off(const uint8_t *p) { return p ? (uint32_t)(p - host_aprom) : 0; }

/* A program of size bytes whose words are seed + index: 0 when it fits. */
static uint32_t install(uint32_t size, uint32_t crc, uint32_t seed)
{
    uint32_t at = xip_begin(size);
    if (!at) return 0;
    for (uint32_t i = 0; i < size / 4u; ++i) {
        uint32_t w = seed + i;
        if (xip_write(at + 4u * i, &w, 1)) return 0;
    }
    return xip_commit(at, size, crc) ? 0 : at;
}

static int intact(uint32_t at, uint32_t size, uint32_t seed)
{
    for (uint32_t i = 0; i < size / 4u; ++i) {
        uint32_t w;
        memcpy(&w, host_aprom + at + 4u * i, 4);
        if (w != seed + i) return 0;
    }
    return 1;
}

static uint32_t used(void)
{
    struct xip_stats st;
    xip_stats(&st);
    return st.used;
}

int main(void)
{
    memset(host_aprom, 0xff, sizeof host_aprom);
    memset(host_aprom, 0x5a, 0x1bf00);           /* the firmware: 16 KiB of area after it */
    host_flash_end = 0x1bf00;
    xip_init();
    struct xip_stats st;
    xip_stats(&st);
    CHECK_EQ_INT(st.base, 0x1c000);
    CHECK_EQ_INT(st.size, 16 * 1024);
    CHECK_EQ_INT(st.used, 0);

    /* Written, found, kept across a reboot. */
    uint32_t a = install(1000, 0xaaaa, 100);
    CHECK_EQ_INT(a, 0x1c000 + XIP_HEADER);
    CHECK_EQ_INT(off(xip_find(0xaaaa, 1000)), a);
    CHECK(!xip_find(0xaaaa, 996));               /* the size must match too */
    CHECK(!xip_find(0xaaab, 1000));
    CHECK_EQ_INT(used(), 3 * PAGE);              /* 32 + 1000 bytes: three pages */
    CHECK(intact(a, 1000, 100));
    xip_init();
    CHECK_EQ_INT(off(xip_find(0xaaaa, 1000)), a);
    uint32_t b = install(200, 0xbbbb, 200);      /* after the newest */
    CHECK_EQ_INT(b, a + 3 * PAGE);
    xip_stats(&st);
    CHECK_EQ_INT(st.writes, 1);                  /* counted since the "boot" */

    /* A slot whose header was never written is no slot: its room is used again. */
    uint32_t c = xip_begin(400);
    CHECK_EQ_INT(c, b + PAGE);
    uint32_t w = 7;
    CHECK_EQ_INT(xip_write(c, &w, 1), 0);
    xip_init();
    CHECK(!xip_find(0xcccc, 400));
    CHECK_EQ_INT(install(400, 0xcccc, 300), c);
    CHECK(xip_find(0xaaaa, 1000) && xip_find(0xbbbb, 200) && xip_find(0xcccc, 400));

    /* Out of room: the oldest gives way, the others stay. The area has 32
     * pages: a 0-2, b 3, c 4, then d 5-16 and e 17-28 (12 each); f (4 pages)
     * does not fit after e, so it wraps to page 0 over a and b. */
    uint32_t d = install(6000, 0xdddd, 400);
    CHECK_EQ_INT(d, 0x1c000 + 5 * PAGE + XIP_HEADER);
    uint32_t e = install(6000, 0xeeee, 500);
    CHECK_EQ_INT(e, 0x1c000 + 17 * PAGE + XIP_HEADER);
    uint32_t f = install(2000, 0xf0f0, 600);
    CHECK_EQ_INT(f, 0x1c000 + XIP_HEADER);
    CHECK(!xip_find(0xaaaa, 1000));
    CHECK(!xip_find(0xbbbb, 200));
    CHECK_EQ_INT(off(xip_find(0xcccc, 400)), c);
    CHECK_EQ_INT(off(xip_find(0xdddd, 6000)), d);
    CHECK_EQ_INT(off(xip_find(0xeeee, 6000)), e);
    CHECK(intact(c, 400, 300) && intact(d, 6000, 400) && intact(e, 6000, 500) && intact(f, 2000, 600));
    xip_init();                                  /* dropped ones stay dropped */
    CHECK(!xip_find(0xaaaa, 1000) && !xip_find(0xbbbb, 200));
    CHECK(xip_find(0xcccc, 400) && xip_find(0xdddd, 6000) && xip_find(0xeeee, 6000) && xip_find(0xf0f0, 2000));

    /* In use: never dropped. With c and d running, 12 pages after f (4-15)
     * would cover them: the room goes past them, over e. */
    struct loaded_image ic = { .slot = c - XIP_HEADER }, id = { .slot = d - XIP_HEADER };
    xip_register(&ic);
    xip_register(&id);
    uint32_t g = install(6000, 0x9999, 700);
    CHECK_EQ_INT(g, e);
    CHECK(!xip_find(0xeeee, 6000));
    CHECK(intact(c, 400, 300) && intact(d, 6000, 400) && intact(g, 6000, 700));
    CHECK_EQ_INT(xip_begin(14000), 0);           /* 28 pages: only by dropping c or d */
    CHECK(xip_find(0xcccc, 400) && xip_find(0xdddd, 6000) && xip_find(0xf0f0, 2000) && xip_find(0x9999, 6000));
    uint32_t addr, size, crc;
    int users = -1, found = 0;
    for (int i = 0; xip_slot(i, &addr, &size, &crc, &users); ++i) {
        if (crc == 0xcccc) { found++; CHECK_EQ_INT(users, 1); CHECK_EQ_INT(addr, c - XIP_HEADER); CHECK_EQ_INT(size, 400); }
        if (crc == 0xf0f0) { found++; CHECK_EQ_INT(users, 0); }
    }
    CHECK_EQ_INT(found, 2);
    xip_release(&ic);
    xip_release(&id);
    uint32_t h = install(14000, 0x7777, 800);    /* now it can */
    CHECK_EQ_INT(h, 0x1c000 + XIP_HEADER);
    CHECK(intact(h, 14000, 800));
    CHECK(!xip_find(0xcccc, 400) && !xip_find(0xdddd, 6000) && !xip_find(0xf0f0, 2000) && !xip_find(0x9999, 6000));

    /* Larger than the area, or a write that fails: no slot. */
    CHECK_EQ_INT(xip_begin(16 * 1024), 0);
    CHECK(xip_begin(16 * 1024 - XIP_HEADER) != 0);
    fail_writes_after = writes + 3;
    CHECK_EQ_INT(install(100, 0x1234, 1), 0);
    fail_writes_after = -1;
    CHECK(!xip_find(0x1234, 100));

    /* Writes stay inside the room; a commit must name it. */
    uint32_t r = xip_begin(100);
    CHECK(r != 0);
    CHECK_EQ_INT(xip_write(r - 4, &w, 1), -1);   /* the header is written by xip_commit only */
    CHECK_EQ_INT(xip_write(r + PAGE - XIP_HEADER, &w, 1), -1);
    CHECK_EQ_INT(xip_write(r + 2, &w, 1), -1);
    CHECK_EQ_INT(xip_commit(r, 600, 0x4321), -1);   /* another size: another span */
    CHECK_EQ_INT(xip_commit(r + PAGE, 100, 0x4321), -1);

    /* A damaged header is not a slot. */
    uint32_t k = install(100, 0x5555, 9);
    CHECK(k && xip_find(0x5555, 100));
    host_aprom[k - XIP_HEADER + 8] ^= 1;         /* crc */
    CHECK(!xip_find(0x5555, 100));
    CHECK(!xip_find(0x5554, 100));

    /* A changed bit in the text: found by its name, but its CRC fails. It is
     * dropped (and stays dropped), unless it is in use. */
    uint32_t m = install(1000, 0x6666, 50);
    CHECK(m && xip_find(0x6666, 1000));
    struct loaded_image im = { .slot = m - XIP_HEADER };
    xip_register(&im);
    host_aprom[m + 600] &= 0xf7;                 /* a bit cleared, as a stray write would */
    CHECK(!xip_find(0x6666, 1000));
    xip_stats(&st);
    CHECK_EQ_INT(st.damaged, 1);
    CHECK(!xip_find(0x6666, 1000));
    found = 0;
    for (int i = 0; xip_slot(i, &addr, &size, &crc, &users); ++i) found += crc == 0x6666;
    CHECK_EQ_INT(found, 1);                      /* still there for the program running from it */
    xip_release(&im);
    CHECK(!xip_find(0x6666, 1000));
    found = 0;
    for (int i = 0; xip_slot(i, &addr, &size, &crc, &users); ++i) found += crc == 0x6666;
    CHECK_EQ_INT(found, 0);
    xip_init();
    CHECK(!xip_find(0x6666, 1000));
    CHECK_EQ_INT(install(1000, 0x6666, 50) != 0, 1);   /* written again */
    CHECK(xip_find(0x6666, 1000));
    return check_summary("test_xip");
}
