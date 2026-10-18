/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NNNIX_KMEM_H
#define NNNIX_KMEM_H
#include <stddef.h>
#include <stdint.h>

/* First-fit allocator with an address-ordered free list and coalescing.
 * Blocks are 8-byte aligned with an 8-byte header (16 on 64-bit hosts). */
#define KMEM_HEADER (sizeof(void *) == 4 ? 8u : 16u)
/* lowest: the least free there was since boot or kmem_reset_lowest(). */
struct kmem_stats { uint32_t total, free, largest, lowest, free_blocks; };

void  kmem_init(void *start, void *end);
void *kmalloc(size_t size);
/* From the top of the heap: for long-lived blocks (login shells), so that they
 * do not split the free space that short-lived allocations come and go in. */
void *kmalloc_high(size_t size);
void  kfree(void *p);
/* Program allocations (sys_alloc) carry the task's pid, so that a program that
 * is killed, or exits without freeing, gives its memory back when it is reaped. */
void  kmem_set_owner(void *p, uint16_t owner);
int   kmem_free_owned(uint16_t owner);          /* returns the number of blocks freed */
int   kmem_grow(void *p, size_t size);              /* in place: 0, or -1 (allocate, copy) */
int   kmem_neighbours(const void *p, uint32_t out[5]);   /* crash reports: p's block and the one before */
int   kmem_reown(uint16_t from, uint16_t to);   /* hand blocks to another task; returns how many */
void  kmem_stats(struct kmem_stats *out);
void  kmem_reset_lowest(void);

#endif
