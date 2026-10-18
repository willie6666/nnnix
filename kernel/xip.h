/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NNNIX_XIP_H
#define NNNIX_XIP_H
#include <stdint.h>

/* Program text in APROM above the firmware, run in place (kernel/xip.c).
 * Each copy is a slot: a header page-aligned, then the text, which the loader
 * relocated for exactly that address. Slots stay across reboots and are found
 * again by the program's text CRC; old ones make room for new ones, oldest
 * first, except while a program runs from them. */
#define XIP_HEADER 32u                       /* a slot: header, then text */

/* A loaded program's RAM block (kernel/exec.c): its data and bss follow (r9
 * points there), and its text too unless that is in APROM. The program and
 * every child it spawns use it, so it is freed with the last of them. */
struct loaded_image {
    uint32_t refs;                   /* tasks using it; changed with interrupts off */
    struct loaded_image *next;       /* the list of images in use */
    uint32_t slot;                   /* the APROM slot holding its text, or 0 */
    uint32_t reserved;               /* keeps the data 8-byte aligned */
};

void xip_init(void);                         /* scan the slots; logs the area */
uint32_t xip_base(void);                     /* the area's start: APROM from here holds only program text */

/* Everything below runs under this lock (the loader's: one program at a time
 * is looked up or written, and registered before the lock is released). */
void xip_lock(void);
void xip_unlock(void);

/* The text of a slot holding this program, or 0. The text is checked against
 * the CRC taken when it was written; a slot that fails is dropped. */
const uint8_t *xip_find(uint32_t crc, uint32_t size);
/* Room for size bytes of text: erases the pages and returns where the text
 * goes, or 0 when no room can be made (every slot in the way is in use, or the
 * text is larger than the area). Slots in the way are dropped first. */
uint32_t xip_begin(uint32_t size);
/* Write words at addr (inside the room), read back: 0, or -1. */
int xip_write(uint32_t addr, const uint32_t *words, uint32_t n);
/* Make the room at text a slot of this program: 0, or -1. */
int xip_commit(uint32_t text, uint32_t size, uint32_t crc);

/* Images in use: a slot one refers to is never dropped. Unregistering also
 * frees the image (task_free, or a failed start). */
void xip_register(struct loaded_image *image);
void xip_release(struct loaded_image *image);

struct xip_stats { uint32_t base, size, used, writes, damaged; };
void xip_stats(struct xip_stats *s);
/* The n-th slot: 1 and its details, or 0 past the last. */
int xip_slot(int n, uint32_t *addr, uint32_t *size, uint32_t *crc, int *users);

#endif
