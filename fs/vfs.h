/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NNNIX_VFS_H
#define NNNIX_VFS_H
#include <stdint.h>
#include "../lib/sys.h"              /* nn_stat, nn_dirent, nn_statfs, O_*, SYS_E* */

/* Virtual file system.
 *
 * Mounts: "/" littlefs on the onboard SPI flash, "/usb" the CH376S drive,
 * "/dev" devices, "/proc" kernel information. Operations on a mount marked
 * `serialized` (littlefs, usbfs) run in the fsd task, one at a time, on fsd's own
 * stack: callers block until fsd is done. Others run in the calling task.
 *
 * Paths given to file system operations are relative to the mount and start
 * with '/'. */
#define VFS_FDS 6                    /* per task; 0-2 fall back to the terminal while unset */

/* lseek's new position, the same rule for every file system: off from 0
 * (SEEK_SET), cur (SEEK_CUR) or end (SEEK_END), within 0..INT32_MAX; else
 * SYS_EINVAL, also for any other whence. Summed without signed overflow
 * (review R63). */
static inline int32_t vfs_seek_pos(uint32_t cur, uint32_t end, int32_t off, int whence)
{
    int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (int64_t)cur : (int64_t)end;
    if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) return SYS_EINVAL;
    int64_t pos = base + off;
    return pos < 0 || pos > INT32_MAX ? SYS_EINVAL : (int32_t)pos;
}

struct file;
struct mount;

struct fs_ops {
    int (*open)(struct mount *m, const char *path, int flags, struct file *f);
    int (*close)(struct file *f);
    int (*read)(struct file *f, void *buf, uint32_t n);
    int (*write)(struct file *f, const void *buf, uint32_t n);
    int (*lseek)(struct file *f, int32_t off, int whence);        /* returns the new position */
    int (*readdir)(struct file *f, struct nn_dirent *d);           /* 1 entry, 0 at the end */
    int (*stat)(struct mount *m, const char *path, struct nn_stat *st);
    int (*mkdir)(struct mount *m, const char *path);
    int (*remove)(struct mount *m, const char *path);
    int (*rename)(struct mount *m, const char *from, const char *to);
    int (*statfs)(struct mount *m, struct nn_statfs *s);
};

struct mount {
    const char *path;                /* "/", "/usb", ... */
    const struct fs_ops *ops;
    void *priv;
    uint8_t serialized;
};

struct file {
    struct mount *mnt;
    void *priv;                      /* owned by the file system */
    uint32_t pos;                    /* for file systems that keep it here */
    uint16_t flags;
    uint8_t refs;
};

void vfs_init(void);                 /* mount table and the fsd task (formats an empty flash) */

/* System-call level interface: paths are absolute or relative to the task's cwd. */
int vfs_open(const char *path, int flags);                      /* fd */
int vfs_close(int fd);
int vfs_read(int fd, void *buf, uint32_t n);
int vfs_write(int fd, const void *buf, uint32_t n);
int vfs_lseek(int fd, int32_t off, int whence);
int vfs_readdir(int fd, struct nn_dirent *d);
int vfs_stat(const char *path, struct nn_stat *st);
int vfs_mkdir(const char *path);
int vfs_unlink(const char *path);
int vfs_rename(const char *from, const char *to);
int vfs_chdir(const char *path);
int vfs_getcwd(char *buf, uint32_t n);
int vfs_statfs(const char *path, struct nn_statfs *s);
int vfs_mkfs(const char *path);      /* "/" only, and only while it is not mounted */
/* Run fn in fsd, which owns the SPI flash: raw flash work (firmware updates)
 * must not interleave with littlefs. */
int vfs_in_fsd(int (*fn)(void *ctx), void *ctx);
int vfs_dup2(int fd, int target);
int vfs_pipe(int fds[2]);            /* read end, write end */
int vfs_install(struct file *f);     /* lowest free fd from 3 */
struct file *vfs_get(int fd);
int vfs_fd_is_open(int fd);          /* for read/write: fd 0-2 unset means the terminal */
void vfs_task_exit(void);            /* close the calling task's files */

/* Build a normalised absolute path (".", "..", repeated slashes resolved). */
int vfs_abspath(const char *cwd, const char *path, char *out, uint32_t size);

/* File systems. */
extern const struct fs_ops lfs_ops, usbfs_ops, devfs_ops, procfs_ops;
int  rootfs_mount(struct mount *m);  /* in fsd: mount; format only a blank flash */
int  rootfs_format(struct mount *m); /* in fsd: format and mount, only while not mounted (or legacy, unused) */
int  rootfs_legacy(void);            /* "/" still covers the firmware update area */
int  usbfs_release(void);            /* before using the CH376S directly: write back, unmount (fsd, CH376S lock) */

#endif
