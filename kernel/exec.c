/* SPDX-License-Identifier: GPL-3.0-or-later */
/* NXE loader (lib/nnlib.h). Runs in the calling task: the file is read through
 * the VFS straight into the program's RAM block, patched and started. */
#include "exec.h"
#include "kmem.h"
#include "../fs/vfs.h"
#include "../lib/string.h"

#define NXE_MAX       (8u * 1024u)   /* image + bss */
#define DEFAULT_STACK 1024u
#define RELOC_CHUNK   16u

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

static int header_ok(const struct nxe_header *h)
{
    return h->magic == NXE_MAGIC && h->version == NXE_VERSION && h->lib_version <= NN_LIB_VERSION &&
           h->image && !(h->image % 4) && !(h->bss % 4) && h->image <= NXE_MAX && h->bss <= NXE_MAX - h->image &&
           h->entry < h->image && (h->entry & 1u) && h->nrelocs <= h->image / 4u &&
           (!h->stack || (h->stack >= 512u && h->stack <= 4096u));
}

int exec_program(const struct nn_exec *req)
{
    if (!req->path || !req->argv || req->argc < 1) return SYS_EINVAL;
    int fd = vfs_open(req->path, O_RDONLY);
    if (fd < 0) return fd;
    struct nxe_header h;
    uint8_t *image = 0;
    uint32_t crc = ~0u;
    int e = read_all(fd, &h, sizeof h);
    if (!e && !header_ok(&h)) e = SYS_ENOEXEC;
    if (!e && !(image = kmalloc(h.image + h.bss))) e = SYS_ENOMEM;
    if (!e) e = read_all(fd, image, h.image);
    if (!e) crc = lfs_crc(crc, image, h.image);
    /* Each listed word holds an address as linked at 0: add where it really is. */
    uint32_t rel[RELOC_CHUNK];
    for (uint32_t done = 0; !e && done < h.nrelocs;) {
        uint32_t k = h.nrelocs - done < RELOC_CHUNK ? h.nrelocs - done : RELOC_CHUNK;
        e = read_all(fd, rel, k * 4u);
        if (e) break;
        crc = lfs_crc(crc, rel, k * 4u);
        for (uint32_t i = 0; i < k && !e; ++i) {
            if (rel[i] % 4u || rel[i] > h.image - 4u) e = SYS_ENOEXEC;
            else *(uint32_t *)(void *)(image + rel[i]) += (uint32_t)(uintptr_t)image;
        }
        done += k;
    }
    if (!e && ~crc != h.crc) e = SYS_ENOEXEC;         /* damaged or truncated upload */
    vfs_close(fd);
    if (e) { kfree(image); return e; }
    memset(image + h.image, 0, h.bss);
    struct nn_spawn sp = { (int (*)(int, char **))(void *)(image + h.entry), req->argv, req->argc,
                           req->fd_in, req->fd_out, h.stack ? h.stack : DEFAULT_STACK };
    int pid = spawn_child(&sp, image);
    if (pid < 0) kfree(image);
    return pid;
}
