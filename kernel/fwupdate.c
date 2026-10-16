/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Firmware updates: copy an APROM image from a file into the update area of
 * the SPI flash and write the record that tells the LDROM bootloader to
 * install it (lib/fwupdate.h, boot/update.c). Runs in the calling task; the
 * flash work itself runs in fsd, which owns the SPI flash. */
#include "fwupdate.h"
#include "kmem.h"
#include "klog.h"
#include "task.h"
#include "../arch/nuc140/arch.h"
#include "../drivers/spiflash.h"
#include "../fs/vfs.h"
#include "../lib/fwupdate.h"
#include "../lib/string.h"

#define CHUNK 256u

uint32_t lfs_crc(uint32_t crc, const void *buffer, size_t size);   /* fs/lfs_port.c */

enum { ERASE, PROGRAM, READ };
struct job { int op; uint32_t addr; void *buf; uint32_t n; };

static int flash_job(void *ctx)
{
    struct job *j = ctx;
    switch (j->op) {
    case ERASE:
        for (uint32_t a = j->addr; a < j->addr + j->n; a += SPIFLASH_SECTOR)
            if (spiflash_erase_sector(a)) return SYS_EIO;
        return 0;
    case PROGRAM:
        return spiflash_program(j->addr, j->buf, j->n) ? SYS_EIO : 0;
    default:
        spiflash_read(j->addr, j->buf, j->n);
        return 0;
    }
}

static int flash(int op, uint32_t addr, void *buf, uint32_t n)
{
    struct job j = { op, addr, buf, n };
    return vfs_in_fsd(flash_job, &j);
}

static int stage(const char *path, uint8_t *buf)
{
    struct nn_stat st;
    int e = vfs_stat(path, &st);
    if (e) return e;
    if (st.type != NN_TYPE_FILE || st.size < 8u || st.size > FW_IMAGE_MAX) return SYS_EINVAL;
    uint32_t size = (st.size + 3u) & ~3u;
    int fd = vfs_open(path, O_RDONLY);
    if (fd < 0) return fd;
    /* The record first: until it is written again, nothing is staged. */
    e = flash(ERASE, FW_RECORD_ADDR, 0, SPIFLASH_SECTOR + ((size + SPIFLASH_SECTOR - 1u) & ~(SPIFLASH_SECTOR - 1u)));
    uint32_t crc = ~0u;
    for (uint32_t off = 0; !e && off < size; off += CHUNK) {
        uint32_t n = size - off < CHUNK ? size - off : CHUNK, got = 0;
        memset(buf, 0xff, n);                        /* pads the last word */
        while (got < n && off + got < st.size) {
            int r = vfs_read(fd, buf + got, n - got);
            if (r <= 0) { e = r ? r : SYS_EIO; break; }
            got += (uint32_t)r;
        }
        if (e) break;
        if (!off) {
            uint32_t sp, reset;
            memcpy(&sp, buf, 4);
            memcpy(&reset, buf + 4, 4);
            if (!fw_vectors_valid(sp, reset, size)) { e = SYS_ENOEXEC; break; }
        }
        crc = lfs_crc(crc, buf, n);
        e = flash(PROGRAM, FW_IMAGE_ADDR + off, buf, n);
    }
    vfs_close(fd);
    crc = ~crc;
    /* Read it back before saying it is there. */
    uint32_t check = ~0u;
    for (uint32_t off = 0; !e && off < size; off += CHUNK) {
        uint32_t n = size - off < CHUNK ? size - off : CHUNK;
        flash(READ, FW_IMAGE_ADDR + off, buf, n);
        check = lfs_crc(check, buf, n);
    }
    if (!e && ~check != crc) e = SYS_EIO;
    if (e) return e;
    struct fw_record r = { FW_MAGIC, size, crc, ~(FW_MAGIC ^ size ^ crc), FW_STAGED };
    e = flash(PROGRAM, FW_RECORD_ADDR, &r, sizeof r);
    if (!e) kprintf("fwupdate: staged %s, %u bytes, crc %08x\n", path, size, crc);
    return e;
}

static int bootloader_present(void) { return fmc_read(0x00100000u) != 0xffffffffu; }

int fw_update(const char *path, int flags, struct nn_fwinfo *info)
{
    if (!path && (flags & NN_FW_REBOOT)) {           /* restart through the bootloader */
        if (!bootloader_present()) return SYS_ENODEV;
        kprintf("fwupdate: restarting through the bootloader\n");
        msleep(100);                                 /* let the log reach the serial port */
        arch_reboot_bootloader();
    }
    if (!path && (flags & NN_FW_CANCEL)) {           /* an erased record stages nothing */
        int e = flash(ERASE, FW_RECORD_ADDR, 0, SPIFLASH_SECTOR);
        if (!e) kprintf("fwupdate: staged image cancelled\n");
        return e;
    }
    if (!path) {
        struct fw_record r;
        memset(info, 0, sizeof *info);
        int e = flash(READ, FW_RECORD_ADDR, &r, sizeof r);
        if (e) return e;
        if (fw_record_valid(&r)) {
            info->state = r.state == FW_STAGED ? NN_FW_STAGED : r.state == FW_DONE ? NN_FW_DONE : NN_FW_NONE;
            info->size = r.size;
            info->crc = r.crc;
        }
        info->bootloader = (uint8_t)bootloader_present();
        info->boot_ldrom = !(fmc_read(0x00300000u) & 0x80u);    /* CONFIG0 CBS: 0 = LDROM */
        info->legacy_fs = (uint8_t)rootfs_legacy();
        return 0;
    }
    if (rootfs_legacy()) return SYS_EBUSY;
    uint8_t *buf = kmalloc(CHUNK);
    if (!buf) return SYS_ENOMEM;
    int e = stage(path, buf);
    kfree(buf);
    return e;
}
