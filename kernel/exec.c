/* SPDX-License-Identifier: GPL-3.0-or-later */
/* NXE loader (lib/nnlib.h). Runs in the calling task. The text runs from an
 * APROM slot (kernel/xip.c): one already holding it, or one written now, its
 * relocations applied on the way; only when APROM has no room does it come to
 * RAM. Data and bss always get RAM, the data read straight into the block.
 * The text and its relocations are checked against text_crc when they are
 * written; a slot found again is taken as it is (it is found by that CRC). */
#include "exec.h"
#include "kmem.h"
#include "task.h"
#include "xip.h"
#include "../fs/vfs.h"
#include "../lib/string.h"

#define TEXT_MAX      (64u * 1024u)
#define DATA_MAX      (8u * 1024u)   /* data + bss */
#define DEFAULT_STACK 1024u
#define CHUNK         16u            /* words read at a time */
#define APROM_FAILED  1              /* load_text: APROM refused a word */

uint32_t lfs_crc(uint32_t crc, const void *buffer, size_t size);   /* fs/lfs_port.c, zlib's CRC-32 */

static int read_all(int fd, void *buf, uint32_t n)
{
    for (uint8_t *p = buf; n;) {
        int got = vfs_read(fd, p, n);
        if (got < 0) return got;
        if (!got) return SYS_ENOEXEC;                /* the file ends early */
        p += got;
        n -= (uint32_t)got;
    }
    return 0;
}

/* The file, read at given offsets: seeks only when the place changes. */
struct file_at { int fd; uint32_t pos; };

static int read_at(struct file_at *f, uint32_t pos, void *buf, uint32_t n)
{
    if (pos != f->pos) {
        int r = vfs_lseek(f->fd, (int32_t)pos, SEEK_SET);
        if (r < 0) return r;
    }
    f->pos = ~0u;
    int e = read_all(f->fd, buf, n);
    if (!e) f->pos = pos + n;
    return e;
}

static int header_ok(const struct nxe_header *h)
{
    return h->magic == NXE_MAGIC && h->version == NXE_VERSION && h->lib_version <= NN_LIB_VERSION &&
           h->text && !(h->text % 4) && h->text <= TEXT_MAX && !(h->data % 4) && !(h->bss % 4) &&
           h->data <= DATA_MAX && h->bss <= DATA_MAX - h->data && h->entry < h->text && (h->entry & 1u) &&
           h->text_relocs <= h->text / 4u && h->data_relocs <= h->data / 4u &&
           (!h->stack || (h->stack >= 512u && h->stack <= 4096u));
}

/* Text relocations, read one at a time from where they are in the file:
 * with -fropi there are few (pointer tables), and this runs on the caller's
 * stack, under the APROM writes. */
struct relocs { uint32_t pos, left, next; };

/* The next offset (they ascend), or ~0u after the last or when reading it
 * failed (the error in *e). */
static uint32_t next_reloc(struct file_at *f, struct relocs *r, int *e)
{
    if (r->next == ~0u && r->left) {
        if ((*e = read_at(f, r->pos, &r->next, 4u))) return r->next = ~0u;
        r->pos += 4u;
        r->left--;
    }
    return r->next;
}

/* The text, relocated for `at`, into APROM (ram == 0, at: the slot's text) or
 * into RAM at ram. Checks it and its relocations against text_crc. */
static int load_text(struct file_at *f, const struct nxe_header *h, uint32_t at, uint8_t *ram)
{
    const uint32_t text_pos = sizeof *h, rel_pos = text_pos + h->text;
    struct relocs r = { .pos = rel_pos, .left = h->text_relocs, .next = ~0u };
    uint32_t words[CHUNK], crc = ~0u, last = 0;
    int e = 0;
    for (uint32_t done = 0; done < h->text;) {
        uint32_t n = h->text - done < sizeof words ? h->text - done : sizeof words;
        if ((e = read_at(f, text_pos + done, words, n))) return e;
        crc = lfs_crc(crc, words, n);
        for (uint32_t off; (off = next_reloc(f, &r, &e)) < done + n;) {
            if (off % 4u || off < last || off < done) return SYS_ENOEXEC;   /* unsorted or doubled */
            words[(off - done) / 4u] += at;
            last = off + 4u;
            r.next = ~0u;
        }
        if (e) return e;
        if (ram) memcpy(ram + done, words, n);
        else if (xip_write(at + done, words, n / 4u)) return APROM_FAILED;
        done += n;
    }
    if (r.left || r.next != ~0u) return SYS_ENOEXEC;       /* a relocation past the end */
    for (uint32_t done = 0; done < h->text_relocs;) {        /* the CRC goes on over them */
        uint32_t k = h->text_relocs - done < CHUNK ? h->text_relocs - done : CHUNK;
        if ((e = read_at(f, rel_pos + 4u * done, words, 4u * k))) return e;
        crc = lfs_crc(crc, words, 4u * k);
        done += k;
    }
    return ~crc == h->text_crc ? 0 : SYS_ENOEXEC;           /* damaged or truncated upload */
}

/* Data into sb, relocated, checked against data_crc. Not inlined: its buffer
 * would sit in exec_program's frame under load_text's. */
__attribute__((noinline)) static int load_data(struct file_at *f, const struct nxe_header *h, const uint8_t *text, uint8_t *sb)
{
    uint32_t pos = sizeof *h + h->text + 4u * h->text_relocs, crc = ~0u, rel[CHUNK];
    int e = read_at(f, pos, sb, h->data);
    if (e) return e;
    crc = lfs_crc(crc, sb, h->data);
    pos += h->data;
    for (uint32_t done = 0; done < h->data_relocs;) {
        uint32_t k = h->data_relocs - done < CHUNK ? h->data_relocs - done : CHUNK;
        if ((e = read_at(f, pos + 4u * done, rel, 4u * k))) return e;
        crc = lfs_crc(crc, rel, 4u * k);
        for (uint32_t i = 0; i < k; ++i) {
            uint32_t off = rel[i] & ~3u;
            if (rel[i] & 2u || off > h->data - 4u) return SYS_ENOEXEC;
            *(uint32_t *)(void *)(sb + off) += (uint32_t)(uintptr_t)((rel[i] & NXE_RELOC_DATA) ? sb : text);
        }
        done += k;
    }
    return ~crc == h->data_crc ? 0 : SYS_ENOEXEC;
}

/* The text in APROM: a slot that has it, or one written now; 0 when there is
 * no room (or writing failed). Under the xip lock. */
static const uint8_t *text_in_aprom(struct file_at *f, const struct nxe_header *h, int *e)
{
    const uint8_t *text = xip_find(h->text_crc, h->text);
    if (text) return text;
    uint32_t at = xip_begin(h->text);
    if (!at) return 0;
    *e = load_text(f, h, at, 0);
    if (*e == APROM_FAILED) *e = 0;                         /* RAM will do */
    else if (!*e && !xip_commit(at, h->text, h->text_crc)) return (const uint8_t *)(uintptr_t)at;
    return 0;
}

int exec_program(const struct nn_exec *req)
{
    if (!req->path || !req->argv || req->argc < 1) return SYS_EINVAL;
    struct file_at f = { vfs_open(req->path, O_RDONLY), 0 };
    if (f.fd < 0) return f.fd;
    struct nxe_header h;
    int e = read_at(&f, 0, &h, sizeof h);
    if (!e && !header_ok(&h)) e = SYS_ENOEXEC;
    /* Exactly the size the header gives: a copy of the text in APROM is found
     * by its CRC without reading the file's, so a cut file would run too. */
    if (!e && vfs_lseek(f.fd, 0, SEEK_END) != (int)(sizeof h + h.text + 4u * h.text_relocs + h.data + 4u * h.data_relocs))
        e = SYS_ENOEXEC;
    f.pos = ~0u;
    if (e) { vfs_close(f.fd); return e; }
    xip_lock();
    const uint8_t *text = text_in_aprom(&f, &h, &e);
    struct loaded_image *block = 0;
    uint32_t ram = h.data + h.bss;
    if (!e && !(block = kmalloc(sizeof *block + ram + (text ? 0 : h.text)))) e = SYS_ENOMEM;
    uint8_t *sb = (uint8_t *)(block + 1);                  /* 8-byte aligned, like the block */
    if (!e) {
        *block = (struct loaded_image){ .slot = text ? (uint32_t)(uintptr_t)text - XIP_HEADER : 0 };
        if (!text) e = load_text(&f, &h, (uint32_t)(uintptr_t)(sb + ram), sb + ram);
        if (!text) text = sb + ram;
    }
    if (!e) e = load_data(&f, &h, text, sb);
    if (!e) xip_register(block);                           /* its slot is kept from now on */
    xip_unlock();
    vfs_close(f.fd);
    if (e) { kfree(block); return e; }
    memset(sb + h.data, 0, h.bss);
    struct nn_spawn sp = { (int (*)(int, char **))(void *)(text + h.entry), req->argv, req->argc,
                           req->fd_in, req->fd_out, h.stack ? h.stack : DEFAULT_STACK };
    int pid = spawn_child(&sp, block);
    if (pid < 0) xip_release(block);
    return pid;
}
