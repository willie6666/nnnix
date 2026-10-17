/* SPDX-License-Identifier: GPL-3.0-or-later */
/* "/": littlefs on the onboard SPI flash (power-loss safe, wear levelled). */
#include <stddef.h>
#include "vfs.h"
#include "../third_party/littlefs/lfs.h"
#include "../drivers/spiflash.h"
#include "../lib/fwupdate.h"
#include "../kernel/klog.h"
#include "../kernel/kmem.h"
#include "../lib/string.h"

#define CACHE 64u

int lfs_port_read(const struct lfs_config *c, lfs_block_t block, lfs_off_t off, void *buffer, lfs_size_t size);
int lfs_port_prog(const struct lfs_config *c, lfs_block_t block, lfs_off_t off, const void *buffer, lfs_size_t size);
int lfs_port_erase(const struct lfs_config *c, lfs_block_t block);
int lfs_port_sync(const struct lfs_config *c);

static lfs_t lfs;
static uint8_t read_cache[CACHE], prog_cache[CACHE], lookahead[16];
static int mounted;
static int legacy;                   /* made before the update area existed: covers the whole flash */
static struct lfs_config cfg = {
    .read = lfs_port_read, .prog = lfs_port_prog, .erase = lfs_port_erase, .sync = lfs_port_sync,
    .read_size = 16, .prog_size = 16, .block_size = SPIFLASH_SECTOR,
    .cache_size = CACHE, .lookahead_size = sizeof lookahead, .block_cycles = 500,
    .read_buffer = read_cache, .prog_buffer = prog_cache, .lookahead_buffer = lookahead,
};

/* An open file or directory; the per-file cache follows the littlefs state.
 * A directory's block ends after `d` (rfs_open): only dir and d are used. */
struct lfile {
    uint8_t dir;
    union { lfs_file_t file; lfs_dir_t d; };
    struct lfs_file_config fcfg;
    uint8_t cache[CACHE];
};

/* littlefs keeps its superblock pair in blocks 0 and 1: all 0xff there means
 * the flash was never formatted. */
static int flash_blank(void)
{
    uint8_t b[32];
    for (uint32_t addr = 0; addr < 2u * SPIFLASH_SECTOR; addr += sizeof b) {
        spiflash_read(addr, b, sizeof b);
        for (unsigned i = 0; i < sizeof b; ++i) if (b[i] != 0xff) return 0;
    }
    return 1;
}

static void mounted_now(uint32_t size)
{
    /* Directories that the other mounts appear in, so "ls /" shows them. */
    static const char *const dirs[] = { "/usb", "/dev", "/proc", "/home", "/etc", "/bin" };
    for (unsigned i = 0; i < sizeof dirs / sizeof dirs[0]; ++i) lfs_mkdir(&lfs, dirs[i]);
    mounted = 1;
    lfs_ssize_t used = lfs_fs_size(&lfs);
    kprintf("rootfs: littlefs, %u KiB, %u KiB used\n", size / 1024u,
            used < 0 ? 0u : (uint32_t)used * SPIFLASH_SECTOR / 1024u);
}

static int format(uint32_t size)
{
    int e = lfs_format(&lfs, &cfg);
    if (!e) e = lfs_mount(&lfs, &cfg);
    if (e) { kprintf("rootfs: format failed (%d)\n", e); return e; }
    mounted_now(size);
    return 0;
}

/* littlefs ends where the firmware update area starts (lib/fwupdate.h). */
static uint32_t fs_bytes(uint32_t size) { return size < FW_FS_END ? size : FW_FS_END; }

/* A blank flash is formatted; any other mount failure (a read error, damage, a
 * different format) leaves the data alone: "/" stays unavailable until
 * someone runs mkfs. A file system from before the update area (the whole
 * flash) is still mounted, but firmware updates wait for mkfs. */
int rootfs_mount(struct mount *m)
{
    (void)m;
    uint32_t size = spiflash_size();
    if (!size) { kprintf("rootfs: no SPI flash\n"); return SYS_ENODEV; }
    cfg.block_count = fs_bytes(size) / SPIFLASH_SECTOR;
    int e = lfs_mount(&lfs, &cfg);
    if (!e) { mounted_now(fs_bytes(size)); return 0; }
    cfg.block_count = 0;                             /* whatever the superblock says */
    if (!lfs_mount(&lfs, &cfg)) {
        if (lfs.block_count * SPIFLASH_SECTOR > fs_bytes(size)) {
            legacy = 1;
            kprintf("rootfs: this file system covers the firmware update area; back up files, then mkfs /\n");
            mounted_now(lfs.block_count * SPIFLASH_SECTOR);
            return 0;
        }
        lfs_unmount(&lfs);
    }
    cfg.block_count = fs_bytes(size) / SPIFLASH_SECTOR;
    if (!flash_blank()) {
        kprintf("rootfs: mount failed (%d); the flash is not blank, so it is left alone (mkfs / formats it)\n", e);
        return e;
    }
    kprintf("rootfs: blank flash, formatting %u KiB\n", fs_bytes(size) / 1024u);
    return format(fs_bytes(size));
}

int rootfs_legacy(void) { return legacy; }

int rootfs_format(struct mount *m)
{
    (void)m;
    /* Only to recover a flash that did not mount, or to move an old file
     * system out of the update area while no file is open on it. */
    if (mounted && !(legacy && !lfs.mlist)) return SYS_EBUSY;
    uint32_t size = spiflash_size();
    if (!size) return SYS_ENODEV;
    if (mounted) { lfs_unmount(&lfs); mounted = 0; legacy = 0; }
    cfg.block_count = fs_bytes(size) / SPIFLASH_SECTOR;
    kprintf("rootfs: formatting %u KiB on request\n", fs_bytes(size) / 1024u);
    return format(fs_bytes(size));
}

static int rfs_open(struct mount *m, const char *path, int flags, struct file *f)
{
    (void)m;
    if (!mounted) return SYS_ENODEV;
    /* A file first, without a separate lfs_stat: each littlefs lookup reads
     * the directories' metadata blocks (tens of ms), and files are what
     * players and copies open. A directory is opened when littlefs says so. */
    if (!(flags & O_DIRECTORY)) {
        struct lfile *lf = kmalloc(sizeof *lf);
        if (!lf) return SYS_ENOMEM;
        memset(lf, 0, sizeof *lf);
        lf->fcfg.buffer = lf->cache;
        int lflags = ((flags & O_ACCMODE) + 1) | (flags & (O_CREAT | O_EXCL | O_TRUNC | O_APPEND));
        int e = lfs_file_opencfg(&lfs, &lf->file, path, lflags, &lf->fcfg);   /* same flag bits */
        if (!e) { f->priv = lf; return 0; }
        kfree(lf);
        if (e != LFS_ERR_ISDIR) return e;
    }
    if ((flags & O_ACCMODE) != O_RDONLY) return SYS_EISDIR;
    /* A directory needs only the flag and the littlefs directory state, not
     * the file state, its config and the 64-byte cache (56 B instead of 164). */
    size_t size = offsetof(struct lfile, d) + sizeof(lfs_dir_t);
    struct lfile *lf = kmalloc(size);
    if (!lf) return SYS_ENOMEM;
    memset(lf, 0, size);
    lf->dir = 1;
    int e = lfs_dir_open(&lfs, &lf->d, path);
    if (e) { kfree(lf); return e; }
    f->priv = lf;
    return 0;
}

static int rfs_close(struct file *f)
{
    struct lfile *lf = f->priv;
    int e = lf->dir ? lfs_dir_close(&lfs, &lf->d) : lfs_file_close(&lfs, &lf->file);
    kfree(lf);
    return e;
}

static int rfs_read(struct file *f, void *buf, uint32_t n)
{
    struct lfile *lf = f->priv;
    return lf->dir ? SYS_EISDIR : lfs_file_read(&lfs, &lf->file, buf, n);
}

static int rfs_write(struct file *f, const void *buf, uint32_t n)
{
    struct lfile *lf = f->priv;
    return lf->dir ? SYS_EISDIR : lfs_file_write(&lfs, &lf->file, buf, n);
}

static int rfs_lseek(struct file *f, int32_t off, int whence)
{
    struct lfile *lf = f->priv;
    return lf->dir ? SYS_EISDIR : lfs_file_seek(&lfs, &lf->file, off, whence);   /* same SEEK_* values */
}

static int rfs_readdir(struct file *f, struct nn_dirent *d)
{
    struct lfile *lf = f->priv;
    struct lfs_info info;
    if (!lf->dir) return SYS_ENOTDIR;
    for (;;) {
        int e = lfs_dir_read(&lfs, &lf->d, &info);
        if (e <= 0) return e;
        if (!strcmp(info.name, ".") || !strcmp(info.name, "..")) continue;
        uint32_t n = (uint32_t)strlen(info.name);
        if (n > NN_NAME_MAX - 1) n = NN_NAME_MAX - 1;
        memcpy(d->name, info.name, n);
        d->name[n] = 0;
        d->size = info.size;
        d->type = info.type == LFS_TYPE_DIR ? NN_TYPE_DIR : NN_TYPE_FILE;
        return 1;
    }
}

static int rfs_stat(struct mount *m, const char *path, struct nn_stat *st)
{
    (void)m;
    struct lfs_info info;
    if (!mounted) return SYS_ENODEV;
    int e = lfs_stat(&lfs, path, &info);
    if (e) return e;
    st->size = info.size;
    st->type = info.type == LFS_TYPE_DIR ? NN_TYPE_DIR : NN_TYPE_FILE;
    return 0;
}

static int rfs_mkdir(struct mount *m, const char *path) { (void)m; return mounted ? lfs_mkdir(&lfs, path) : SYS_ENODEV; }
static int rfs_remove(struct mount *m, const char *path) { (void)m; return mounted ? lfs_remove(&lfs, path) : SYS_ENODEV; }
static int rfs_rename(struct mount *m, const char *from, const char *to)
{
    (void)m;
    return mounted ? lfs_rename(&lfs, from, to) : SYS_ENODEV;
}

static int rfs_statfs(struct mount *m, struct nn_statfs *s)
{
    (void)m;
    if (!mounted) return SYS_ENODEV;
    lfs_ssize_t used = lfs_fs_size(&lfs);
    if (used < 0) return (int)used;
    s->block_size = cfg.block_size;
    s->blocks = lfs.block_count;                     /* cfg.block_count is 0 for a legacy layout */
    s->free_blocks = lfs.block_count - (uint32_t)used;
    return 0;
}

const struct fs_ops lfs_ops = {
    .open = rfs_open, .close = rfs_close, .read = rfs_read, .write = rfs_write, .lseek = rfs_lseek,
    .readdir = rfs_readdir, .stat = rfs_stat, .mkdir = rfs_mkdir, .remove = rfs_remove,
    .rename = rfs_rename, .statfs = rfs_statfs,
};
