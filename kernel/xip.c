/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Program text in APROM (xip.h). The area runs from the firmware's end,
 * rounded up to a page, to the end of APROM: a firmware update that grows
 * overwrites only the slots at its start, and those are no longer looked at.
 *
 * A slot is a 32-byte header at a page start, then the text, in whole pages.
 * The header is written last, its check word last of all, so a slot whose
 * writing was cut short (a reset, power lost) is not one. A slot is dropped by
 * clearing its live word (a write that only clears bits needs no erase)
 * before any of its pages is erased. Erasing a page holds interrupts off for
 * 20 ms (arch/nuc140/fmc.c); other tasks run between pages. */
#include <stddef.h>
#include "xip.h"
#include "kmem.h"

#ifdef NNNIX_HOST
/* tests/host/test_xip.c: APROM is an array there, and the test supplies the
 * flash controller. */
extern uint8_t host_aprom[];
extern uint32_t host_flash_end;
#define AP(a) (host_aprom + (a))
#define FLASH_END host_flash_end
#define FMC_PAGE 512u
int fmc_erase(uint32_t page);
int fmc_write(uint32_t addr, uint32_t word);
#define kprintf(...) ((void)0)
#define task_yield() ((void)0)
#define irq_save() 0u
#define irq_restore(s) ((void)(s))
void xip_lock(void) { }
void xip_unlock(void) { }
#else
#include "klog.h"
#include "sync.h"
#include "task.h"
#include "../arch/nuc140/arch.h"
extern const uint8_t __flash_end[];          /* arch/nuc140/link.ld */
#define AP(a) ((const uint8_t *)(uintptr_t)(a))
#define FLASH_END ((uint32_t)(uintptr_t)__flash_end)
static struct mutex lock;
void xip_lock(void) { mutex_lock(&lock); }
void xip_unlock(void) { mutex_unlock(&lock); }
#endif

#define XIP_MAGIC 0x31504958u                /* "XIP1" */
#define AP_END    0x20000u                   /* 128 KiB of APROM */
#define HEADER    XIP_HEADER

struct slot_header { uint32_t magic, size, crc, seq, check, live, reserved[2]; };
_Static_assert(sizeof(struct slot_header) == HEADER, "slot header size");

static uint32_t base;                        /* the area's first page */
static uint32_t next_seq, next_at;           /* the next slot's number, and where it goes first */
static uint32_t room_at, room_end;           /* pages erased for the slot being written */
static uint32_t slots_written;
static struct loaded_image *images;          /* every image a task uses or is about to */

static const struct slot_header *header(uint32_t a) { return (const struct slot_header *)(const void *)AP(a); }
static uint32_t check_of(const struct slot_header *h) { return ~(h->magic ^ h->size ^ h->crc ^ h->seq); }
static uint32_t span(uint32_t size) { return (HEADER + size + FMC_PAGE - 1u) / FMC_PAGE * FMC_PAGE; }

/* The slot at a, or 0 when that page does not start one: its bytes. */
static uint32_t slot_span(uint32_t a)
{
    const struct slot_header *h = header(a);
    if (h->magic != XIP_MAGIC || h->check != check_of(h) || h->live != ~0u || !h->size || h->size >= AP_END) return 0;
    uint32_t n = span(h->size);
    return a + n <= AP_END ? n : 0;
}

/* The next slot at or after *a: its address, with *a moved past it; 0 at the end. */
static uint32_t next_slot(uint32_t *a)
{
    while (*a < AP_END) {
        uint32_t s = *a, n = slot_span(s);
        if (n) { *a = s + n; return s; }
        *a = s + FMC_PAGE;
    }
    return 0;
}

static int users(uint32_t slot)
{
    int n = 0;
    uint32_t s = irq_save();
    for (struct loaded_image *i = images; i; i = i->next) if (i->slot == slot) ++n;
    irq_restore(s);
    return n;
}

void xip_init(void)
{
    base = (FLASH_END + FMC_PAGE - 1u) / FMC_PAGE * FMC_PAGE;
    next_seq = slots_written = 0;
    next_at = base;
    uint32_t count = 0, newest = 0;
    for (uint32_t a = base, s; (s = next_slot(&a));) {
        ++count;
        if (header(s)->seq >= newest) { newest = header(s)->seq; next_seq = newest + 1u; next_at = a; }
    }
    kprintf("xip: APROM 0x%05x-0x%05x (%u KiB) for programs, %u kept\n", base, AP_END, (AP_END - base) / 1024u, count);
}

const uint8_t *xip_find(uint32_t crc, uint32_t size)
{
    for (uint32_t a = base, s; (s = next_slot(&a));)
        if (header(s)->crc == crc && header(s)->size == size) return AP(s + HEADER);
    return 0;
}

/* The end of a slot in use that overlaps [from, to), or 0 when none does. */
static uint32_t in_use_within(uint32_t from, uint32_t to)
{
    for (uint32_t a = base, s; (s = next_slot(&a));)
        if (s < to && a > from && users(s)) return a;
    return 0;
}

uint32_t xip_begin(uint32_t size)
{
    room_at = room_end = 0;
    if (base >= AP_END || size > AP_END - base - HEADER) return 0;
    uint32_t need = span(size), at = next_at;
    for (int wrapped = 0;;) {
        if (at + need > AP_END) {
            if (wrapped++) return 0;
            at = base;
        }
        uint32_t busy = in_use_within(at, at + need);
        if (!busy) break;
        at = busy;
    }
    for (uint32_t a = base, s; (s = next_slot(&a));)       /* drop the slots in the way */
        if (s < at + need && a > at && fmc_write(s + offsetof(struct slot_header, live), 0)) return 0;
    for (uint32_t p = at; p < at + need; p += FMC_PAGE) {
        if (fmc_erase(p)) return 0;
        task_yield();                                      /* interrupts were off for 20 ms */
    }
    room_at = at;
    room_end = at + need;
    return at + HEADER;
}

int xip_write(uint32_t addr, const uint32_t *words, uint32_t n)
{
    if (addr < room_at + HEADER || addr % 4u || n > (room_end - addr) / 4u) return -1;
    for (uint32_t i = 0; i < n; ++i, addr += 4u) {
        if (words[i] != ~0u && fmc_write(addr, words[i])) return -1;   /* erased words already read ~0 */
        if (*(volatile const uint32_t *)(const void *)AP(addr) != words[i]) return -1;
    }
    return 0;
}

int xip_commit(uint32_t text, uint32_t size, uint32_t crc)
{
    uint32_t s = text - HEADER;
    if (s != room_at || span(size) != room_end - room_at) return -1;
    struct slot_header h = { XIP_MAGIC, size, crc, next_seq, 0, ~0u, { ~0u, ~0u } };
    h.check = check_of(&h);
    room_at = room_end = 0;
    const uint32_t *w = (const uint32_t *)&h;
    for (uint32_t i = 0; i <= offsetof(struct slot_header, check) / 4u; ++i)   /* check last */
        if (fmc_write(s + 4u * i, w[i])) return -1;
    if (!slot_span(s)) return -1;
    ++next_seq;
    next_at = s + span(size);
    ++slots_written;
    return 0;
}

void xip_register(struct loaded_image *image)
{
    uint32_t s = irq_save();
    image->next = images;
    images = image;
    irq_restore(s);
}

void xip_release(struct loaded_image *image)
{
    if (!image) return;
    uint32_t s = irq_save();
    for (struct loaded_image **p = &images; *p; p = &(*p)->next)
        if (*p == image) { *p = image->next; break; }
    irq_restore(s);
    kfree(image);
}

void xip_stats(struct xip_stats *st)
{
    *st = (struct xip_stats){ .base = base, .size = base < AP_END ? AP_END - base : 0, .writes = slots_written };
    for (uint32_t a = base, s; (s = next_slot(&a));) st->used += a - s;
}

int xip_slot(int n, uint32_t *addr, uint32_t *size, uint32_t *crc, int *count)
{
    for (uint32_t a = base, s; (s = next_slot(&a));) {
        if (n--) continue;
        *addr = s;
        *size = header(s)->size;
        *crc = header(s)->crc;
        *count = users(s);
        return 1;
    }
    return 0;
}
