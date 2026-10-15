/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NNNIX_KMEM_H
#define NNNIX_KMEM_H
#include <stddef.h>
#include <stdint.h>

/* First-fit allocator with an address-ordered free list and coalescing.
 * Blocks are 8-byte aligned with an 8-byte header (16 on 64-bit hosts). */
#define KMEM_HEADER (sizeof(void *) == 4 ? 8u : 16u)
struct kmem_stats { uint32_t total, free, largest, used_blocks, free_blocks; };

void  kmem_init(void *start, void *end);
void *kmalloc(size_t size);
void  kfree(void *p);
void  kmem_stats(struct kmem_stats *out);

#endif
