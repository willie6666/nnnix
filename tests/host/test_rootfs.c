/* SPDX-License-Identifier: GPL-3.0-or-later */
/* rootfs mount policy on a RAM flash with the real littlefs: a blank flash is
 * formatted, a damaged one is left alone until mkfs. Each boot is a forked
 * child (fresh static state) on one shared flash image. */
#define _DEFAULT_SOURCE
#include "../../fs/vfs.h"
#include "../../drivers/spiflash.h"
#include "../../lib/fwupdate.h"
#include "../../third_party/littlefs/lfs.h"
#include "check.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define FLASH (2048u * 1024u)
static uint32_t flash_size = 64u * 1024u;
static uint8_t *flash;               /* shared between the parent and the boots */
static char log_text[2048];

uint32_t spiflash_size(void) { return flash_size; }
int spiflash_read(uint32_t addr, void *buf, uint32_t n) { memcpy(buf, flash + addr, n); return 0; }
int spiflash_program(uint32_t addr, const void *buf, uint32_t n)
{
    const uint8_t *p = buf;
    for (uint32_t i = 0; i < n; ++i) flash[addr + i] &= p[i];      /* NOR: bits only go to 0 */
    return 0;
}
int spiflash_erase_sector(uint32_t addr) { memset(flash + addr, 0xff, SPIFLASH_SECTOR); return 0; }
void *kmalloc(size_t n) { return malloc(n); }
void kfree(void *p) { free(p); }
void kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    size_t k = strlen(log_text);
    vsnprintf(log_text + k, sizeof log_text - k, fmt, ap);
    va_end(ap);
}

static struct mount root = { "/", &lfs_ops, 0, 1 };

static int write_file(const char *path, const char *text)
{
    struct file f = { .mnt = &root };
    int e = lfs_ops.open(&root, path, O_WRONLY | O_CREAT | O_TRUNC, &f);
    if (e) return e;
    e = lfs_ops.write(&f, text, (uint32_t)strlen(text));
    return lfs_ops.close(&f) ? -1 : e;
}

static int read_file(const char *path, char *buf, uint32_t n)
{
    struct file f = { .mnt = &root };
    int e = lfs_ops.open(&root, path, O_RDONLY, &f);
    if (e) return e;
    int got = lfs_ops.read(&f, buf, n - 1);
    lfs_ops.close(&f);
    buf[got > 0 ? got : 0] = 0;
    return got;
}

/* One boot in a child; returns its exit code (the number of failed checks). */
static int boot(int (*scenario)(void))
{
    pid_t pid = fork();
    if (!pid) { int failed = scenario(); if (failed) fputs(log_text, stderr); _exit(failed); }
    int status;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 99;
}

static int blank_boot(void)
{
    CHECK_EQ_INT(rootfs_mount(&root), 0);
    CHECK(strstr(log_text, "blank flash, formatting") != NULL);
    CHECK_EQ_INT(write_file("/home/keep", "precious"), 8);
    return check_failures;
}

static int normal_boot(void)
{
    char buf[32];
    CHECK_EQ_INT(rootfs_mount(&root), 0);
    CHECK(strstr(log_text, "format") == NULL);
    CHECK_EQ_INT(read_file("/home/keep", buf, sizeof buf), 8);
    CHECK(!strcmp(buf, "precious"));
    CHECK_EQ_INT(rootfs_format(&root), SYS_EBUSY);   /* never while mounted */
    CHECK_EQ_INT(read_file("/home/keep", buf, sizeof buf), 8);
    return check_failures;
}

static uint8_t *before;

static int damaged_boot(void)
{
    CHECK(rootfs_mount(&root) != 0);
    CHECK(strstr(log_text, "left alone") != NULL);
    CHECK(!memcmp(flash, before, flash_size));       /* nothing written */
    struct file f = { .mnt = &root };
    CHECK_EQ_INT(lfs_ops.open(&root, "/home/keep", O_RDONLY, &f), SYS_ENODEV);
    CHECK_EQ_INT(rootfs_format(&root), 0);           /* mkfs / */
    CHECK_EQ_INT(write_file("/home/new", "fresh"), 5);
    return check_failures;
}

/* 2 MiB, as on the board: the update area stays untouched. */
static int big_blank_boot(void)
{
    struct nn_statfs st;
    CHECK_EQ_INT(rootfs_mount(&root), 0);
    CHECK_EQ_INT(lfs_ops.statfs(&root, &st), 0);
    CHECK_EQ_INT(st.blocks, FW_FS_END / SPIFLASH_SECTOR);
    CHECK_EQ_INT(rootfs_legacy(), 0);
    for (int i = 0; i < 20; ++i) {
        char name[32];
        snprintf(name, sizeof name, "/home/f%d", i);
        CHECK_EQ_INT(write_file(name, "some data for the file system"), 29);
    }
    for (uint32_t a = FW_FS_END; a < flash_size; ++a) if (flash[a] != 0xff) { CHECK(!"update area written"); break; }
    return check_failures;
}

int lfs_port_read(const struct lfs_config *c, lfs_block_t block, lfs_off_t off, void *buffer, lfs_size_t size);
int lfs_port_prog(const struct lfs_config *c, lfs_block_t block, lfs_off_t off, const void *buffer, lfs_size_t size);
int lfs_port_erase(const struct lfs_config *c, lfs_block_t block);
int lfs_port_sync(const struct lfs_config *c);

/* A file system as older firmware made it: the whole flash. */
static int make_legacy(void)
{
    static uint8_t rc[64], pc[64], la[16];
    struct lfs_config c = { .read = lfs_port_read, .prog = lfs_port_prog, .erase = lfs_port_erase,
        .sync = lfs_port_sync, .read_size = 16, .prog_size = 16, .block_size = SPIFLASH_SECTOR,
        .block_count = flash_size / SPIFLASH_SECTOR, .cache_size = 64, .lookahead_size = 16, .block_cycles = 500,
        .read_buffer = rc, .prog_buffer = pc, .lookahead_buffer = la };
    lfs_t l;
    CHECK_EQ_INT(lfs_format(&l, &c), 0);
    CHECK_EQ_INT(lfs_mount(&l, &c), 0);
    lfs_mkdir(&l, "/home");
    lfs_file_t f;
    static uint8_t fbuf[64];
    struct lfs_file_config fc = { .buffer = fbuf };
    CHECK_EQ_INT(lfs_file_opencfg(&l, &f, "/home/old", LFS_O_WRONLY | LFS_O_CREAT, &fc), 0);
    CHECK_EQ_INT(lfs_file_write(&l, &f, "kept", 4), 4);
    lfs_file_close(&l, &f);
    lfs_unmount(&l);
    return check_failures;
}

static int legacy_boot(void)
{
    char buf[16];
    struct nn_statfs st;
    CHECK_EQ_INT(rootfs_mount(&root), 0);
    CHECK_EQ_INT(rootfs_legacy(), 1);
    CHECK(strstr(log_text, "covers the firmware update area") != NULL);
    CHECK_EQ_INT(read_file("/home/old", buf, sizeof buf), 4);   /* files can still be copied off */
    CHECK_EQ_INT(lfs_ops.statfs(&root, &st), 0);
    CHECK_EQ_INT(st.blocks, flash_size / SPIFLASH_SECTOR);
    struct file f = { .mnt = &root };
    CHECK_EQ_INT(lfs_ops.open(&root, "/home/old", O_RDONLY, &f), 0);
    CHECK_EQ_INT(rootfs_format(&root), SYS_EBUSY);   /* a file is open */
    lfs_ops.close(&f);
    CHECK_EQ_INT(rootfs_format(&root), 0);
    CHECK_EQ_INT(rootfs_legacy(), 0);
    CHECK_EQ_INT(lfs_ops.statfs(&root, &st), 0);
    CHECK_EQ_INT(st.blocks, FW_FS_END / SPIFLASH_SECTOR);
    CHECK_EQ_INT(rootfs_format(&root), SYS_EBUSY);   /* mounted normally now */
    return check_failures;
}

static int after_migration_boot(void)
{
    CHECK_EQ_INT(rootfs_mount(&root), 0);
    CHECK_EQ_INT(rootfs_legacy(), 0);
    CHECK(strstr(log_text, "format") == NULL);
    return check_failures;
}

int main(void)
{
    flash = mmap(0, FLASH, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    before = malloc(FLASH);
    memset(flash, 0xff, FLASH);
    CHECK_EQ_INT(boot(blank_boot), 0);
    CHECK_EQ_INT(boot(normal_boot), 0);
    for (uint32_t b = 0; b < 2; ++b)                  /* damage both superblocks */
        for (uint32_t i = 0; i < 64; ++i) flash[b * SPIFLASH_SECTOR + i] ^= 0x5a;
    memcpy(before, flash, FLASH);
    CHECK_EQ_INT(boot(damaged_boot), 0);

    flash_size = FLASH;
    memset(flash, 0xff, FLASH);
    CHECK_EQ_INT(boot(big_blank_boot), 0);
    memset(flash, 0xff, FLASH);
    CHECK_EQ_INT(boot(make_legacy), 0);
    CHECK_EQ_INT(boot(legacy_boot), 0);
    CHECK_EQ_INT(boot(after_migration_boot), 0);
    return check_summary("test_rootfs");
}
