/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NNNIX_CH376_H
#define NNNIX_CH376_H
#include <stdint.h>

/* CH376S USB host + FAT, SPI mode. One file open at a time (chip limitation).
 * Names are 8.3, upper case. A name starting with '/' opens from the root
 * directory; one without opens inside the directory opened last, so a path
 * is walked one component at a time. */
#define CH376_OK          0x14
#define CH376_OPEN_DIR    0x41
#define CH376_MISS_FILE   0x42
#define CH376_TIMEOUT     0xff
#define CH376_ENTRY_READY 0x1d
#define CH376_SPI_ERROR   0xfc       /* ours: the SPI transfer failed (spi.h) */

/* Multi-step operations (mount, open, read/write, close) must hold the device lock. */
void     ch376_lock(void);
int      ch376_trylock(void);
void     ch376_unlock(void);
int      ch376_present(void);                 /* CHECK_EXIST round trip */
uint8_t  ch376_version(void);
int      ch376_mount(void);                   /* 0 or error stage 1..4 */
uint8_t  ch376_open(const char *path);         /* CH376_OK, CH376_OPEN_DIR, CH376_MISS_FILE, ... */
uint8_t  ch376_create(const char *path);
uint8_t  ch376_close(int update_size);
uint8_t  ch376_erase(const char *path);
uint8_t  ch376_file_size(uint32_t *size);     /* of the file open; CH376_OK */
uint8_t  ch376_write(const void *buf, uint32_t len);            /* CH376_OK when all written */
uint8_t  ch376_read(void *buf, uint32_t len, uint32_t *got);     /* CH376_OK at end of request */
uint8_t  ch376_byte_locate(uint32_t offset);
uint8_t  ch376_dir_create(const char *name);
uint8_t  ch376_erase_open(void);                 /* remove the file or empty directory just opened */
uint8_t  ch376_enum_first(const char *pattern);
uint8_t  ch376_enum_next(void);
int      ch376_read_entry(uint8_t entry[32]);   /* the entry's length, or -1 */
uint8_t  ch376_disk_query(uint32_t *total_sectors, uint32_t *free_sectors);

void ch376_isr(void);                /* INT edge (board interrupt) */
uint32_t ch376_commands(void);       /* commands sent so far: unchanged means the chip state is too */

#endif
