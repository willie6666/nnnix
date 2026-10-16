/* SPDX-License-Identifier: GPL-3.0-or-later */
/* rootfs mount policy on a RAM flash with the real littlefs: a blank flash is
 * formatted, a damaged one is left alone until mkfs. Each boot is a forked
 * child (fresh static state) on one shared flash image. */
#define _DEFAULT_SOURCE
#include "../../fs/vfs.h"
#include "../../drivers/spiflash.h"
#include "check.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define FLASH (64u * 1024u)
static uint8_t *flash;               /* shared between the parent and the boots */
static char log_text[2048];

uint32_t spiflash_size(void) { return FLASH; }
void spiflash_read(uint32_t addr, void *buf, uint32_t n) { memcpy(buf, flash + addr, n); }
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
    CHECK(!memcmp(flash, before, FLASH));            /* nothing written */
    struct file f = { .mnt = &root };
    CHECK_EQ_INT(lfs_ops.open(&root, "/home/keep", O_RDONLY, &f), SYS_ENODEV);
    CHECK_EQ_INT(rootfs_format(&root), 0);           /* mkfs / */
    CHECK_EQ_INT(write_file("/home/new", "fresh"), 5);
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
    return check_summary("test_rootfs");
}
