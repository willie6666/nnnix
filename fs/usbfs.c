/* SPDX-License-Identifier: GPL-3.0-or-later */
/* "/usb": the FAT drive behind the CH376S, which can only have one file open.
 * Each open file remembers its 8.3 path and position; the chip is switched to
 * whichever file is being used (reopen + seek). Names are case-insensitive:
 * they are stored upper case and listed lower case. Runs in fsd. */
#include "vfs.h"
#include "../drivers/ch376.h"
#include "../kernel/klog.h"
#include "../kernel/kmem.h"
#include "../kernel/modules.h"
#include "../lib/string.h"

#define MAX_DEPTH 6

struct ufile {
    char path[NN_PATH_MAX];          /* 8.3 upper case, absolute within the drive */
    uint32_t size;
    uint8_t dir, dirty;
    int8_t error;                    /* a write-back that failed while switching files, for close */
};

static struct ufile *current;        /* file the chip has open */
static uint8_t mounted;

static int valid_char(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr("_-~!#$%&'()@^{}", c);
}

/* "/dir/file.txt" -> "/DIR/FILE.TXT", checking every component is 8.3 and
 * that there are at most MAX_DEPTH of them (walk() has room for that many). */
static int to83(const char *in, char *out)
{
    uint32_t o = 0, depth = 0;
    while (*in) {
        while (*in == '/') ++in;
        if (!*in) break;
        uint32_t base = 0, ext = 0, dot = 0;
        if (o + 14 >= NN_PATH_MAX || ++depth > MAX_DEPTH) return SYS_ENAMETOOLONG;
        out[o++] = '/';
        for (; *in && *in != '/'; ++in) {
            char c = *in >= 'a' && *in <= 'z' ? (char)(*in - 32) : *in;
            if (c == '.') { if (dot || !base) return SYS_EINVAL; dot = 1; out[o++] = c; continue; }
            if (!valid_char(c)) return SYS_EINVAL;
            if (dot ? ++ext > 3 : ++base > 8) return SYS_ENAMETOOLONG;
            out[o++] = c;
        }
        if (dot && !ext) return SYS_EINVAL;
    }
    if (!o) out[o++] = '/';
    out[o] = 0;
    return 0;
}

/* Open the first `count` components of path; returns the CH376 status of the
 * last one (CH376_OK file, CH376_OPEN_DIR directory, CH376_MISS_FILE). */
static uint8_t walk(const char *path, int skip_last)
{
    char copy[NN_PATH_MAX], name[14];
    char *comp[MAX_DEPTH];
    int n = 0;
    strcpy(copy, path);
    for (char *p = copy; *p;) {
        while (*p == '/') *p++ = 0;
        if (!*p) break;
        if (n == MAX_DEPTH) return 0xf1;              /* to83 rejects this; never copy past name[] */
        comp[n++] = p;
        while (*p && *p != '/') ++p;
    }
    n -= skip_last;
    if (n <= 0) return ch376_open("/");
    uint8_t s = 0;
    for (int i = 0; i < n; ++i) {
        uint32_t k = 0;
        if (i == 0) name[k++] = '/';
        strcpy(name + k, comp[i]);
        s = ch376_open(name);
        if (i + 1 < n && s != CH376_OPEN_DIR) {
            if (s == CH376_OK) ch376_close(0);
            return s == CH376_OK ? 0xf0 : s;           /* 0xf0: a file used as a directory */
        }
    }
    return s;
}

static const char *last_name(const char *path) { const char *s = strchr(path, 0); while (s > path && s[-1] != '/') --s; return s; }

static int status_error(uint8_t s)
{
    switch (s) {
    case CH376_MISS_FILE: return SYS_ENOENT;
    case 0xf0: return SYS_ENOTDIR;
    case 0xf1: return SYS_ENAMETOOLONG;
    case 0x1e: case 0x1d: return SYS_EIO;
    default: return SYS_EIO;
    }
}

/* Close the chip's file; with dirty data this writes the new size to the
 * directory entry, so a failure is kept for that file's close(). */
static void deactivate(void)
{
    if (!current) return;
    if (ch376_close(current->dirty) != CH376_OK && current->dirty) current->error = SYS_EIO;
    current->dirty = 0;
    current = 0;
}

/* Before something else drives the CH376S directly (the diag benchmark resets
 * it): write back the chip's open file and forget the mount, so the next use
 * mounts again and reopens its file. In fsd, with the CH376S lock held. A
 * failed write-back is also kept for that file's close. */
int usbfs_release(void)
{
    struct ufile *u = current;
    deactivate();
    mounted = 0;
    return u ? u->error : 0;
}

static int ensure(void)
{
    if (!modules.ch376) return SYS_ENODEV;
    if (mounted) return 0;
    current = 0;
    int e = ch376_mount();
    if (e) { kprintf("usbfs: mount failed (stage %d)\n", e); return SYS_ENODEV; }
    mounted = 1;
    kprintf("usbfs: drive mounted\n");
    return 0;
}

static int activate(struct ufile *u)
{
    if (current == u) return 0;
    deactivate();
    uint8_t s = walk(u->path, 0);
    if (s != CH376_OK) { mounted = s == CH376_OK; return status_error(s); }
    current = u;
    /* Another handle may have changed the file since; its data was written
     * back when it was deactivated, so the directory entry is current. */
    u->size = ch376_file_size();
    return 0;
}

static int u_open(struct mount *m, const char *path, int flags, struct file *f)
{
    (void)m;
    struct ufile *u = kmalloc(sizeof *u);
    if (!u) return SYS_ENOMEM;
    memset(u, 0, sizeof *u);
    int e = to83(path, u->path);
    ch376_lock();
    if (!e) e = ensure();
    if (!e) {
        deactivate();
        uint8_t s = walk(u->path, 0);
        int writable = (flags & O_ACCMODE) != O_RDONLY;
        if (s == CH376_OPEN_DIR) {
            u->dir = 1;
            if (writable) e = SYS_EISDIR;
        } else if (s == CH376_OK) {
            u->size = ch376_file_size();
            ch376_close(0);
            if ((flags & O_CREAT) && (flags & O_EXCL)) e = SYS_EEXIST;
            else if (flags & O_DIRECTORY) e = SYS_ENOTDIR;
            else if (writable && (flags & O_TRUNC) && u->size) {
                if (walk(u->path, 0) != CH376_OK || ch376_erase_open() != CH376_OK) e = SYS_EIO;
                s = CH376_MISS_FILE;
            }
        } else if (s != CH376_MISS_FILE) {
            e = status_error(s);
        } else if (!(flags & O_CREAT)) {
            e = SYS_ENOENT;
        }
        if (!e && s == CH376_MISS_FILE) {          /* create */
            uint8_t p = walk(u->path, 1);
            if (p != CH376_OPEN_DIR) e = p == CH376_MISS_FILE ? SYS_ENOENT : status_error(p);
            else if (ch376_create(last_name(u->path)) != CH376_OK) e = SYS_EIO;
            else { ch376_close(1); u->size = 0; }
        }
    }
    ch376_unlock();
    if (e) { kfree(u); return e; }
    f->priv = u;
    f->pos = (flags & O_APPEND) ? u->size : 0;
    return 0;
}

static int u_close(struct file *f)
{
    struct ufile *u = f->priv;
    ch376_lock();
    if (current == u) deactivate();
    ch376_unlock();
    int e = u->error;
    kfree(u);
    return e;
}

static int u_read(struct file *f, void *buf, uint32_t n)
{
    struct ufile *u = f->priv;
    uint32_t got = 0;
    if (u->dir) return SYS_EISDIR;
    ch376_lock();
    int e = ensure();
    if (!e) e = activate(u);                        /* refreshes the size */
    if (!e && f->pos >= u->size) { ch376_unlock(); return 0; }
    if (!e && n > u->size - f->pos) n = u->size - f->pos;
    if (!e && ch376_byte_locate(f->pos) != CH376_OK) e = SYS_EIO;
    if (!e && ch376_read(buf, n, &got) != CH376_OK) e = SYS_EIO;
    ch376_unlock();
    if (e) return e;
    f->pos += got;
    return (int)got;
}

static int u_write(struct file *f, const void *buf, uint32_t n)
{
    struct ufile *u = f->priv;
    if (u->dir) return SYS_EISDIR;
    ch376_lock();
    int e = ensure();
    if (!e) e = activate(u);                        /* refreshes the size */
    if (!e && (f->flags & O_APPEND)) f->pos = u->size;
    if (!e && ch376_byte_locate(f->pos) != CH376_OK) e = SYS_EIO;
    if (!e && ch376_write(buf, n) != CH376_OK) e = SYS_EIO;
    if (!e) u->dirty = 1;
    ch376_unlock();
    if (e) return e;
    f->pos += n;
    if (f->pos > u->size) u->size = f->pos;
    return (int)n;
}

static int u_lseek(struct file *f, int32_t off, int whence)
{
    struct ufile *u = f->priv;
    if (whence == SEEK_END && !u->dir) {               /* the current size, as read and write see it */
        ch376_lock();
        int e = ensure();
        if (!e) e = activate(u);
        ch376_unlock();
        if (e) return e;
    }
    int32_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (int32_t)f->pos : (int32_t)u->size;
    int32_t pos = base + off;
    if (pos < 0) return SYS_EINVAL;
    f->pos = (uint32_t)pos > u->size ? u->size : (uint32_t)pos;   /* no holes on FAT through the CH376 */
    return (int)f->pos;
}

/* Directory entries are listed by enumerating from the start and skipping the
 * ones already returned: no chip state survives between calls. */
static int u_readdir(struct file *f, struct nn_dirent *d)
{
    struct ufile *u = f->priv;
    uint8_t ent[32];
    if (!u->dir) return SYS_ENOTDIR;
    ch376_lock();
    int e = ensure(), found = 0;
    if (!e) {
        deactivate();
        uint8_t s = walk(u->path, 0);
        uint32_t index = 0;
        s = s == CH376_OPEN_DIR ? ch376_enum_first(strcmp(u->path, "/") ? "*" : "/*") : CH376_MISS_FILE;
        while (s == CH376_ENTRY_READY) {
            ch376_read_entry(ent);
            int skip = ent[0] == 0xe5 || ent[0] == '.' || (ent[11] & 0x08) || ent[11] == 0x0f;
            if (!skip && index++ == f->pos) {
                uint32_t k = 0;
                for (int i = 0; i < 8 && ent[i] != ' '; ++i) d->name[k++] = (char)(ent[i] >= 'A' && ent[i] <= 'Z' ? ent[i] + 32 : ent[i]);
                if (ent[8] != ' ') {
                    d->name[k++] = '.';
                    for (int i = 8; i < 11 && ent[i] != ' '; ++i) d->name[k++] = (char)(ent[i] >= 'A' && ent[i] <= 'Z' ? ent[i] + 32 : ent[i]);
                }
                d->name[k] = 0;
                d->type = (ent[11] & 0x10) ? NN_TYPE_DIR : NN_TYPE_FILE;
                d->size = ent[28] | (uint32_t)ent[29] << 8 | (uint32_t)ent[30] << 16 | (uint32_t)ent[31] << 24;
                found = 1;
                f->pos++;
                /* Stop enumerating: the chip forgets the listing at the next command anyway. */
                break;
            }
            s = ch376_enum_next();
        }
    }
    ch376_unlock();
    return e ? e : found;
}

static int u_stat(struct mount *m, const char *path, struct nn_stat *st)
{
    (void)m;
    char p83[NN_PATH_MAX];
    int e = to83(path, p83);
    if (e) return e;
    ch376_lock();
    e = ensure();
    if (!e) {
        deactivate();
        uint8_t s = walk(p83, 0);
        if (s == CH376_OPEN_DIR) { st->type = NN_TYPE_DIR; st->size = 0; }
        else if (s == CH376_OK) { st->type = NN_TYPE_FILE; st->size = ch376_file_size(); ch376_close(0); }
        else e = status_error(s);
    }
    ch376_unlock();
    return e;
}

static int u_mkdir(struct mount *m, const char *path)
{
    struct nn_stat st;
    char p83[NN_PATH_MAX];
    int e = to83(path, p83);
    if (e) return e;
    if (!u_stat(m, path, &st)) return SYS_EEXIST;
    ch376_lock();
    e = ensure();
    if (!e) {
        uint8_t s = walk(p83, 1);
        if (s != CH376_OPEN_DIR) e = s == CH376_MISS_FILE ? SYS_ENOENT : status_error(s);
        else if (ch376_dir_create(last_name(p83)) != CH376_OK) e = SYS_EIO;
        else ch376_close(0);
    }
    ch376_unlock();
    return e;
}

static int u_remove(struct mount *m, const char *path)
{
    struct nn_stat st;
    char p83[NN_PATH_MAX];
    int e = to83(path, p83);
    if (!e) e = u_stat(m, path, &st);
    if (e) return e;
    if (st.type == NN_TYPE_DIR) {                    /* only empty directories */
        struct ufile dir = { .dir = 1 };
        struct file f = { .priv = &dir };
        struct nn_dirent d;
        strcpy(dir.path, p83);
        if (u_readdir(&f, &d) > 0) return SYS_ENOTEMPTY;
    }
    ch376_lock();
    e = ensure();
    if (!e) {
        deactivate();
        uint8_t s = walk(p83, 0);
        if (s != CH376_OK && s != CH376_OPEN_DIR) e = status_error(s);
        else if (ch376_erase_open() != CH376_OK) e = SYS_EIO;
    }
    ch376_unlock();
    return e;
}

static int u_statfs(struct mount *m, struct nn_statfs *s)
{
    (void)m;
    uint32_t total = 0, free = 0;
    ch376_lock();
    int e = ensure();
    if (!e) { deactivate(); if (ch376_disk_query(&total, &free) != CH376_OK) e = SYS_EIO; }
    ch376_unlock();
    if (e) return e;
    s->block_size = 512;
    s->blocks = total;
    s->free_blocks = free;
    return 0;
}

const struct fs_ops usbfs_ops = {
    .open = u_open, .close = u_close, .read = u_read, .write = u_write, .lseek = u_lseek,
    .readdir = u_readdir, .stat = u_stat, .mkdir = u_mkdir, .remove = u_remove, .statfs = u_statfs,
};
