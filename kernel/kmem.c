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
#define USED 0x55534544u             /* "USED": catches double frees and stray pointers */

struct block {
    uint32_t size;                   /* including the header */
    union { struct block *next; uint32_t tag; };
};
/* 8 bytes on the Cortex-M0; 16 on a 64-bit host where the tests run. */
#define HDR ((uint32_t)((sizeof(struct block) + 7u) & ~(size_t)7u))
_Static_assert(KMEM_HEADER == HDR, "KMEM_HEADER out of date");

static struct block *free_list;
static uint32_t total;

void kmem_init(void *start, void *end)
{
    uintptr_t a = ((uintptr_t)start + 7u) & ~(uintptr_t)7u;
    uintptr_t b = (uintptr_t)end & ~(uintptr_t)7u;
    free_list = 0;
    total = 0;
    if (b <= a + HDR + MIN_SPLIT) return;
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
        b->tag = USED;
    }
    UNLOCK(s);
    return b ? (char *)b + HDR : 0;
}

void kfree(void *p)
{
    if (!p) return;
    struct block *b = (struct block *)((char *)p - HDR);
    uint32_t s = LOCK();
    if (b->tag != USED) { UNLOCK(s); return; }  /* double free or foreign pointer: ignore */
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
