/* SPDX-License-Identifier: GPL-3.0-or-later */
/* usbfs against a CH376S model: one open file at a time, relative opens walk
 * from the directory opened last, and a file's directory entry (its size)
 * changes only when it is closed with update_size. */
#include "../../fs/vfs.h"
#include "../../drivers/ch376.h"
#include "../../kernel/modules.h"
#include "check.h"
#include <stdlib.h>
#include <string.h>

struct mfile { char path[NN_PATH_MAX]; int used, dir; uint8_t data[256]; uint32_t size; };
static struct mfile disk[32];
static int cur = -1, fail_close;
static uint32_t commands, enum_steps;   /* every chip command; listing steps */
static int enum_at = -1;                 /* disk index of the entry being listed */
uint32_t ch376_commands(void) { return commands; }
static char dir[NN_PATH_MAX];        /* directory opened last; "" is the root */
static uint32_t pos, live;           /* the open file's position and size as the chip knows them */

struct modules_state modules = { .ch376 = 1 };
void *kmalloc(size_t n) { return malloc(n); }
void kfree(void *p) { free(p); }
void kprintf(const char *fmt, ...) { (void)fmt; }
void ch376_lock(void) { }
void ch376_unlock(void) { }
int ch376_mount(void) { cur = -1; return 0; }  /* RESET_ALL: the chip's open file is gone */

static int find(const char *path)
{
    for (int i = 0; i < 32; ++i) if (disk[i].used && !strcmp(disk[i].path, path)) return i;
    return -1;
}
static void full(const char *name, char *out)
{
    if (name[0] == '/') strcpy(out, name);
    else { strcpy(out, dir); strcat(out, "/"); strcat(out, name); }
}
static int add(const char *path, int is_dir)
{
    for (int i = 0; i < 32; ++i)
        if (!disk[i].used) { memset(&disk[i], 0, sizeof disk[i]); disk[i].used = 1; disk[i].dir = is_dir; strcpy(disk[i].path, path); return i; }
    return -1;
}

uint8_t ch376_open(const char *name)
{
    ++commands;
    enum_at = -1;
    char p[NN_PATH_MAX * 2];
    full(name, p);
    cur = -1;
    if (!strcmp(p, "/")) { dir[0] = 0; return CH376_OPEN_DIR; }
    int i = find(p);
    if (i < 0) return CH376_MISS_FILE;
    if (disk[i].dir) { strcpy(dir, p); return CH376_OPEN_DIR; }
    cur = i; pos = 0; live = disk[i].size;
    return CH376_OK;
}
uint8_t ch376_create(const char *name)
{
    char p[NN_PATH_MAX * 2];
    full(name, p);
    cur = add(p, 0); pos = live = 0;
    return cur < 0 ? CH376_TIMEOUT : CH376_OK;
}
uint8_t ch376_dir_create(const char *name)
{
    char p[NN_PATH_MAX * 2];
    full(name, p);
    return add(p, 1) < 0 ? CH376_TIMEOUT : CH376_OK;
}
uint8_t ch376_close(int update_size)
{
    ++commands;
    enum_at = -1;
    if (fail_close) { cur = -1; return CH376_TIMEOUT; }
    if (update_size && cur >= 0) disk[cur].size = live;
    cur = -1;
    return CH376_OK;
}
uint32_t ch376_file_size(void) { return live; }
uint8_t ch376_byte_locate(uint32_t offset) { pos = offset > live ? live : offset; return CH376_OK; }
uint8_t ch376_write(const void *buf, uint32_t len)
{
    if (cur < 0 || pos + len > sizeof disk[cur].data) return CH376_TIMEOUT;
    memcpy(disk[cur].data + pos, buf, len);
    pos += len;
    if (pos > live) live = pos;
    return CH376_OK;
}
uint8_t ch376_read(void *buf, uint32_t len, uint32_t *got)
{
    *got = len < live - pos ? len : live - pos;
    memcpy(buf, disk[cur].data + pos, *got);
    pos += *got;
    return CH376_OK;
}
uint8_t ch376_erase_open(void) { if (cur >= 0) disk[cur].used = 0; cur = -1; return CH376_OK; }
/* Listing the directory opened last: the entries whose parent it is. */
static int in_dir(int i)
{
    const char *slash = strrchr(disk[i].path, '/');
    return disk[i].used && (size_t)(slash - disk[i].path) == strlen(dir) && !strncmp(disk[i].path, dir, strlen(dir));
}
static uint8_t enum_from(int i)
{
    ++enum_steps;
    while (i < 32 && !in_dir(i)) ++i;
    enum_at = i < 32 ? i : -1;
    return enum_at >= 0 ? CH376_ENTRY_READY : CH376_MISS_FILE;
}
uint8_t ch376_enum_first(const char *pattern) { (void)pattern; ++commands; return enum_from(0); }
uint8_t ch376_enum_next(void) { ++commands; return enum_at < 0 ? CH376_MISS_FILE : enum_from(enum_at + 1); }
int ch376_read_entry(uint8_t entry[32])
{
    ++commands;
    memset(entry, ' ', 11);
    memset(entry + 11, 0, 21);
    const char *n = strrchr(disk[enum_at].path, '/') + 1, *dot = strchr(n, '.');
    for (int i = 0; i < 8 && n[i] && n + i != dot; ++i) entry[i] = (uint8_t)n[i];
    if (dot) for (int i = 0; i < 3 && dot[1 + i]; ++i) entry[8 + i] = (uint8_t)dot[1 + i];
    entry[11] = disk[enum_at].dir ? 0x10 : 0x20;
    entry[28] = (uint8_t)disk[enum_at].size;
    return 0;
}
uint8_t ch376_disk_query(uint32_t *total, uint32_t *free_sectors) { *total = *free_sectors = 0; return CH376_OK; }

static struct mount usb = { "/usb", &usbfs_ops, 0, 1 };

static int open_file(const char *path, int flags, struct file *f)
{
    memset(f, 0, sizeof *f);
    f->mnt = &usb;
    f->flags = (uint16_t)flags;
    return usbfs_ops.open(&usb, path, flags, f);
}

int main(void)
{
    struct file a, b;
    char buf[64];

    /* R7: at most six path components; never a longer copy into walk()'s name. */
    const char *dirs[] = { "/A", "/A/B", "/A/B/C", "/A/B/C/D", "/A/B/C/D/E", "/A/B/C/D/E/F" };
    for (int i = 0; i < 6; ++i) CHECK_EQ_INT(usbfs_ops.mkdir(&usb, dirs[i]), 0);
    CHECK_EQ_INT(usbfs_ops.mkdir(&usb, "/A/B/C/D/E/F/G"), SYS_ENAMETOOLONG);
    CHECK_EQ_INT(open_file("/A/B/C/D/E/F/12345678.ABC", O_RDONLY, &a), SYS_ENAMETOOLONG);
    CHECK_EQ_INT(open_file("/a/b/c/d/e/f/x.txt", O_WRONLY | O_CREAT, &a), SYS_ENAMETOOLONG);
    struct nn_stat st;
    CHECK_EQ_INT(usbfs_ops.stat(&usb, "/A/B/C/D/E/F/G/H/I/J.TXT", &st), SYS_ENAMETOOLONG);
    CHECK_EQ_INT(open_file("/A/B/C/D/E/X.TXT", O_WRONLY | O_CREAT, &a), 0);
    CHECK_EQ_INT(usbfs_ops.close(&a), 0);

    /* R11: two appending handles see each other's data. */
    CHECK_EQ_INT(open_file("/LOG.TXT", O_WRONLY | O_CREAT | O_APPEND, &a), 0);
    CHECK_EQ_INT(open_file("/LOG.TXT", O_WRONLY | O_APPEND, &b), 0);
    CHECK_EQ_INT(usbfs_ops.write(&a, "A", 1), 1);
    CHECK_EQ_INT(usbfs_ops.write(&b, "B", 1), 1);
    CHECK_EQ_INT(usbfs_ops.write(&a, "C", 1), 1);
    CHECK_EQ_INT(usbfs_ops.lseek(&b, 0, SEEK_END), 3);
    CHECK_EQ_INT(usbfs_ops.close(&a), 0);
    CHECK_EQ_INT(usbfs_ops.close(&b), 0);
    CHECK_EQ_INT(open_file("/LOG.TXT", O_RDONLY, &a), 0);
    CHECK_EQ_INT(usbfs_ops.read(&a, buf, sizeof buf), 3);
    CHECK(!memcmp(buf, "ABC", 3));
    CHECK_EQ_INT(usbfs_ops.close(&a), 0);

    /* R9: a failed write-back is reported by close, also when it happened
     * while switching to another file. */
    CHECK_EQ_INT(open_file("/X.TXT", O_WRONLY | O_CREAT, &a), 0);
    CHECK_EQ_INT(usbfs_ops.write(&a, "data", 4), 4);
    fail_close = 1;
    CHECK_EQ_INT(usbfs_ops.close(&a), SYS_EIO);
    fail_close = 0;
    CHECK_EQ_INT(open_file("/Y.TXT", O_WRONLY | O_CREAT, &a), 0);
    CHECK_EQ_INT(open_file("/LOG.TXT", O_RDONLY, &b), 0);
    CHECK_EQ_INT(usbfs_ops.write(&a, "data", 4), 4);
    fail_close = 1;
    CHECK_EQ_INT(usbfs_ops.read(&b, buf, sizeof buf), 3);  /* switches files: a's write-back fails */
    fail_close = 0;
    CHECK_EQ_INT(usbfs_ops.close(&b), 0);
    CHECK_EQ_INT(usbfs_ops.close(&a), SYS_EIO);
    CHECK_EQ_INT(open_file("/Y.TXT", O_RDONLY, &a), 0);  /* a clean file closes cleanly */
    CHECK_EQ_INT(usbfs_ops.close(&a), 0);

    /* R35: before the diag bench resets the chip, the open file is written
     * back; afterwards its handles carry on (a remount and reopen). */
    CHECK_EQ_INT(open_file("/D.TXT", O_WRONLY | O_CREAT, &a), 0);
    CHECK_EQ_INT(open_file("/LOG.TXT", O_RDONLY, &b), 0);
    CHECK_EQ_INT(usbfs_ops.write(&a, "dirty", 5), 5);
    CHECK_EQ_INT(usbfs_release(), 0);
    ch376_mount();                                    /* the bench */
    CHECK_EQ_INT(disk[find("/D.TXT")].size, 5);
    CHECK_EQ_INT(usbfs_ops.read(&b, buf, sizeof buf), 3);
    CHECK_EQ_INT(usbfs_ops.write(&a, "!", 1), 1);
    CHECK_EQ_INT(usbfs_ops.close(&a), 0);
    CHECK_EQ_INT(usbfs_ops.close(&b), 0);
    CHECK_EQ_INT(disk[find("/D.TXT")].size, 6);
    /* A failed write-back stops the bench and is reported by close too. */
    CHECK_EQ_INT(open_file("/E.TXT", O_WRONLY | O_CREAT, &a), 0);
    CHECK_EQ_INT(usbfs_ops.write(&a, "data", 4), 4);
    fail_close = 1;
    CHECK_EQ_INT(usbfs_release(), SYS_EIO);
    fail_close = 0;
    CHECK_EQ_INT(usbfs_ops.close(&a), SYS_EIO);
    CHECK_EQ_INT(usbfs_release(), 0);                 /* nothing open: nothing to write back */

    /* O9: a listing goes on where it stopped (N steps for N entries) while
     * nothing else uses the chip, and starts again correctly when something does. */
    CHECK_EQ_INT(usbfs_ops.mkdir(&usb, "/L"), 0);
    for (int i = 0; i < 10; ++i) {
        char name[16];
        snprintf(name, sizeof name, "/L/F%d.TXT", i);
        CHECK_EQ_INT(open_file(name, O_WRONLY | O_CREAT, &a), 0);
        CHECK_EQ_INT(usbfs_ops.close(&a), 0);
    }
    struct nn_dirent de;
    char seen[200] = "";
    CHECK_EQ_INT(open_file("/L", O_RDONLY, &a), 0);
    enum_steps = 0;
    while (usbfs_ops.readdir(&a, &de) == 1) { strcat(seen, de.name); strcat(seen, " "); }
    CHECK(!strcmp(seen, "f0.txt f1.txt f2.txt f3.txt f4.txt f5.txt f6.txt f7.txt f8.txt f9.txt "));
    CHECK_EQ_INT(enum_steps, 11);                     /* ten entries and the end */
    CHECK_EQ_INT(usbfs_ops.close(&a), 0);
    CHECK_EQ_INT(open_file("/L", O_RDONLY, &a), 0);
    CHECK_EQ_INT(open_file("/LOG.TXT", O_RDONLY, &b), 0);
    seen[0] = 0;
    for (int i = 0; usbfs_ops.readdir(&a, &de) == 1; ++i) {
        strcat(seen, de.name); strcat(seen, " ");
        if (i == 3) CHECK_EQ_INT(usbfs_ops.read(&b, buf, sizeof buf), 3);   /* the chip is used meanwhile */
    }
    CHECK(!strcmp(seen, "f0.txt f1.txt f2.txt f3.txt f4.txt f5.txt f6.txt f7.txt f8.txt f9.txt "));
    CHECK_EQ_INT(usbfs_ops.close(&b), 0);
    CHECK_EQ_INT(usbfs_ops.close(&a), 0);
    return check_summary("test_usbfs");
}
