/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "kmem.h"

#ifdef NNNIX_HOST
#define LOCK() 0u
#define UNLOCK(s) (void)(s)
#else
#include "../arch/nuc140/arch.h"
#define LOCK() irq_save()
#define UNLOCK(s) irq_restore(s)
#endif

#define MIN_SPLIT 16u
/* A used block's tag: "US" in the high half catches double frees and stray
 * pointers; the low half is the owning task's pid, or 0 for the kernel. */
#define USED_MAGIC 0x5553u
#define USED(owner) ((uint32_t)USED_MAGIC << 16 | (owner))
#define IS_USED(b) ((b)->tag >> 16 == USED_MAGIC)

struct block {
    uint32_t size;                   /* including the header */
    union { struct block *next; uint32_t tag; };
};
/* 8 bytes on the Cortex-M0; 16 on a 64-bit host where the tests run. */
#define HDR ((uint32_t)((sizeof(struct block) + 7u) & ~(size_t)7u))
_Static_assert(KMEM_HEADER == HDR, "KMEM_HEADER out of date");

static struct block *free_list;
static uint32_t total;
static char *heap_base, *heap_limit;

void kmem_init(void *start, void *end)
{
    uintptr_t a = ((uintptr_t)start + 7u) & ~(uintptr_t)7u;
    uintptr_t b = (uintptr_t)end & ~(uintptr_t)7u;
    free_list = 0;
    total = 0;
    heap_base = heap_limit = (char *)a;
    if (b <= a + HDR + MIN_SPLIT) return;
    heap_limit = (char *)b;
    free_list = (struct block *)a;
    free_list->size = (uint32_t)(b - a);
    free_list->next = 0;
    total = free_list->size;
}

void *kmalloc(size_t size)
{
    if (!size || total < HDR || size > total - HDR) return 0;
    uint32_t need = (uint32_t)((size + 7u) & ~(size_t)7u) + HDR;
    uint32_t s = LOCK();
    struct block **link = &free_list, *b;
    while ((b = *link) && b->size < need) link = &b->next;
    if (b) {
        if (b->size - need >= MIN_SPLIT + HDR) {
            struct block *rest = (struct block *)((char *)b + need);
            rest->size = b->size - need;
            rest->next = b->next;
            *link = rest;
            b->size = need;
        } else {
            *link = b->next;
        }
        b->tag = USED(0);
    }
    UNLOCK(s);
    return b ? (char *)b + HDR : 0;
}

void *kmalloc_high(size_t size)
{
    if (!size || total < HDR || size > total - HDR) return 0;
    uint32_t need = (uint32_t)((size + 7u) & ~(size_t)7u) + HDR;
    uint32_t s = LOCK();
    struct block **link = &free_list, **best = 0, *b;
    for (; (b = *link); link = &b->next)            /* the list is in address order: keep the last fit */
        if (b->size >= need) best = link;
    b = best ? *best : 0;
    if (b) {
        if (b->size - need >= MIN_SPLIT + HDR) {
            b->size -= need;                            /* the free part stays where it is */
            b = (struct block *)((char *)b + b->size);
            b->size = need;
        } else {
            *best = b->next;
        }
        b->tag = USED(0);
    }
    UNLOCK(s);
    return b ? (char *)b + HDR : 0;
}

void kfree(void *p)
{
    if (!p) return;
    struct block *b = (struct block *)((char *)p - HDR);
    uint32_t s = LOCK();
    if (!IS_USED(b)) { UNLOCK(s); return; }     /* double free or foreign pointer: ignore */
    struct block **link = &free_list, *next;
    while ((next = *link) && next < b) link = &next->next;
    b->next = next;
    *link = b;
    if (next && (char *)b + b->size == (char *)next) {      /* merge with the following block */
        b->size += next->size;
        b->next = next->next;
    }
    if (link != &free_list) {                                /* merge with the preceding block */
        struct block *prev = (struct block *)((char *)link - offsetof(struct block, next));
        if ((char *)prev + prev->size == (char *)b) {
            prev->size += b->size;
            prev->next = b->next;
        }
    }
    UNLOCK(s);
}

void kmem_set_owner(void *p, uint16_t owner)
{
    struct block *b = (struct block *)((char *)p - HDR);
    uint32_t s = LOCK();
    if (IS_USED(b)) b->tag = USED(owner);
    UNLOCK(s);
}

/* The blocks tile the heap, so walk them by size; free blocks are recognised
 * by the (address-ordered) free list rather than by their contents. */
static void *find_owned(uint16_t owner)
{
    struct block *f = free_list;
    for (char *p = heap_base; p < heap_limit; p += ((struct block *)p)->size) {
        struct block *b = (struct block *)p;
        if (b == f) { f = f->next; continue; }
        if (b->tag == USED(owner)) return p + HDR;
    }
    return 0;
}

int kmem_reown(uint16_t from, uint16_t to)
{
    int n = 0;
    uint32_t s = LOCK();
    struct block *f = free_list;
    for (char *p = heap_base; p < heap_limit; p += ((struct block *)p)->size) {
        struct block *b = (struct block *)p;
        if (b == f) { f = f->next; continue; }
        if (b->tag == USED(from)) { b->tag = USED(to); ++n; }
    }
    UNLOCK(s);
    return n;
}

int kmem_free_owned(uint16_t owner)
{
    int n = 0;
    if (!owner) return 0;
    for (;;) {
        uint32_t s = LOCK();
        void *p = find_owned(owner);
        UNLOCK(s);
        if (!p) return n;
        kfree(p);
        ++n;
    }
}

void kmem_stats(struct kmem_stats *out)
{
    uint32_t s = LOCK();
    out->total = total;
    out->free = out->largest = out->free_blocks = 0;
    for (struct block *b = free_list; b; b = b->next) {
        out->free += b->size;
        if (b->size > out->largest) out->largest = b->size;
        out->free_blocks++;
    }
    UNLOCK(s);
    out->used_blocks = 0;            /* not tracked; kept for the ps/meminfo layout */
}

/* For crash reports: the header words of the block holding p and of the
 * block before it (out[0..1] that one's address and size, out[2] its tag
 * or free-list link, out[3..4] p's block size and tag). 0 if not found. */
int kmem_neighbours(const void *p, uint32_t out[5])
{
    char *prev = 0;
    for (char *b = heap_base; b < heap_limit;) {
        const struct block *h = (const struct block *)(void *)b;
        if (h->size < HDR || b + h->size > heap_limit) return 0;   /* a broken header */
        if ((const char *)p >= b && (const char *)p < b + h->size) {
            const struct block *q = (const struct block *)(void *)prev;
            out[0] = (uint32_t)(uintptr_t)prev;
            out[1] = q ? q->size : 0;
            out[2] = q ? q->tag : 0;
            out[3] = h->size;
            out[4] = h->tag;
            return 1;
        }
        prev = b;
        b += h->size;
    }
    return 0;
}
