/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NNNIX_CH376_H
#define NNNIX_CH376_H
#include <stdint.h>

/* CH376S USB host + FAT, SPI mode. One file open at a time (chip limitation).
 * Paths: "/NAME.EXT" in the root directory (8.3, upper case). */
#define CH376_OK          0x14
#define CH376_OPEN_DIR    0x41
#define CH376_MISS_FILE   0x42
#define CH376_TIMEOUT     0xff

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
uint32_t ch376_file_size(void);
uint8_t  ch376_write(const void *buf, uint32_t len);            /* CH376_OK when all written */
uint8_t  ch376_read(void *buf, uint32_t len, uint32_t *got);     /* CH376_OK at end of request */

#endif
