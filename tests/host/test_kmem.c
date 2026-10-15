/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../../kernel/kmem.h"
#include "check.h"

static _Alignas(8) unsigned char heap[4096];

static uint32_t free_bytes(void)
{
    struct kmem_stats s;
    kmem_stats(&s);
    return s.free;
}

int main(void)
{
    kmem_init(heap, heap + sizeof heap);
    struct kmem_stats s;
    kmem_stats(&s);
    CHECK_EQ_INT(s.total, sizeof heap);
    CHECK_EQ_INT(s.free_blocks, 1);

    void *a = kmalloc(100), *b = kmalloc(200), *c = kmalloc(300);
    CHECK(a && b && c);
    CHECK_EQ_INT((uintptr_t)a % 8, 0);
    CHECK_EQ_INT((uintptr_t)b % 8, 0);
    CHECK((char *)b >= (char *)a + 100);
    memset(a, 0xaa, 100); memset(b, 0xbb, 200); memset(c, 0xcc, 300);
    CHECK_EQ_INT(free_bytes(), sizeof heap - (104 + KMEM_HEADER) - (200 + KMEM_HEADER) - (304 + KMEM_HEADER));

    /* Free the middle block, then its neighbours: everything must coalesce back. */
    kfree(b);
    kmem_stats(&s);
    CHECK_EQ_INT(s.free_blocks, 2);
    void *b2 = kmalloc(150);                 /* first fit reuses the hole */
    CHECK(b2 == b);
    kfree(b2);
    kfree(a);
    kmem_stats(&s);
    CHECK_EQ_INT(s.free_blocks, 2);          /* a+b merged, tail after c */
    kfree(c);
    kmem_stats(&s);
    CHECK_EQ_INT(s.free_blocks, 1);
    CHECK_EQ_INT(s.free, sizeof heap);

    kfree(c);                                /* double free is ignored */
    CHECK_EQ_INT(free_bytes(), sizeof heap);
    CHECK(kmalloc(0) == NULL);
    CHECK(kmalloc(sizeof heap) == NULL);     /* header does not fit */
    CHECK(kmalloc(SIZE_MAX) == NULL);
    CHECK(kmalloc(UINT32_MAX) == NULL);
    CHECK_EQ_INT(free_bytes(), sizeof heap);
    void *all = kmalloc(sizeof heap - KMEM_HEADER);
    CHECK(all != NULL);
    CHECK(kmalloc(8) == NULL);
    kfree(all);

    /* Random churn with content checks. */
    void *ptr[32] = { 0 };
    size_t len[32] = { 0 };
    srand(1);
    for (int round = 0; round < 20000; ++round) {
        int i = rand() % 32;
        if (ptr[i]) {
            for (size_t k = 0; k < len[i]; ++k)
                if (((unsigned char *)ptr[i])[k] != (unsigned char)i) { CHECK(!"block corrupted"); round = 20000; break; }
            kfree(ptr[i]);
            ptr[i] = NULL;
        } else {
            len[i] = (size_t)(rand() % 300) + 1;
            ptr[i] = kmalloc(len[i]);
            if (ptr[i]) memset(ptr[i], i, len[i]);
        }
    }
    for (int i = 0; i < 32; ++i) kfree(ptr[i]);
    kmem_stats(&s);
    CHECK_EQ_INT(s.free, sizeof heap);
    CHECK_EQ_INT(s.free_blocks, 1);
    return check_summary("test_kmem");
}
