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
            ptr[i] = (i & 1) ? kmalloc_high(len[i]) : kmalloc(len[i]);
            if (ptr[i]) memset(ptr[i], i, len[i]);
        }
    }
    for (int i = 0; i < 32; ++i) kfree(ptr[i]);
    kmem_stats(&s);
    CHECK_EQ_INT(s.free, sizeof heap);
    CHECK_EQ_INT(s.free_blocks, 1);

    /* Long-lived blocks come from the top: freeing and reallocating a low block
     * around them leaves the free space in one piece. */
    char *low = kmalloc(100), *high = kmalloc_high(200);
    CHECK(low == (char *)heap + KMEM_HEADER || (uintptr_t)low - (uintptr_t)heap < 16);
    CHECK((char *)high + 200 + 8 >= (char *)heap + sizeof heap - 8);
    kfree(low);
    kmem_stats(&s);
    CHECK_EQ_INT(s.free_blocks, 1);
    CHECK_EQ_INT(s.largest, sizeof heap - KMEM_HEADER - 200);
    char *exact = kmalloc_high(s.largest - KMEM_HEADER);   /* takes the whole free block */
    CHECK(exact != NULL);
    kmem_stats(&s);
    CHECK_EQ_INT(s.free, 0);
    kfree(exact);
    kfree(high);
    kmem_stats(&s);
    CHECK_EQ_INT(s.free, sizeof heap);
    CHECK_EQ_INT(s.free_blocks, 1);
    CHECK(kmalloc_high(sizeof heap) == NULL);

    /* Owned blocks: a task's leftovers are found by walking the heap and freed;
     * other owners' and kernel blocks stay. */
    char *k1 = kmalloc(40), *a1 = kmalloc(24), *b1 = kmalloc(56), *a2 = kmalloc_high(200), *a3 = kmalloc(8);
    CHECK(k1 && a1 && b1 && a2 && a3);
    kmem_set_owner(a1, 7); kmem_set_owner(a2, 7); kmem_set_owner(a3, 7); kmem_set_owner(b1, 9);
    kfree(a3);                                         /* freed by the program itself */
    kfree(a3);                                         /* double free: ignored */
    CHECK_EQ_INT(kmem_free_owned(7), 2);
    CHECK_EQ_INT(kmem_free_owned(7), 0);
    CHECK_EQ_INT(kmem_free_owned(0), 0);               /* kernel blocks are never "owned" */
    memset(k1, 0x11, 40); memset(b1, 0x22, 56);        /* still usable */
    CHECK_EQ_INT(kmem_reown(9, 12), 1);               /* handed on: 9 owns nothing now */
    CHECK_EQ_INT(kmem_free_owned(9), 0);
    CHECK_EQ_INT(kmem_free_owned(12), 1);
    kfree(k1);
    kmem_stats(&s);
    CHECK_EQ_INT(s.free, sizeof heap);
    CHECK_EQ_INT(s.free_blocks, 1);
    return check_summary("test_kmem");
}
