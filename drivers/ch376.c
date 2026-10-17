/* SPDX-License-Identifier: GPL-3.0-or-later */
/* CH376S driver. Data bytes are sent one SPI transaction at a time: back-to-back
 * 32-bit bursts were measured to corrupt data. INT (PB14) signals completion. */
#include "ch376.h"
#include "spi.h"
#include "../arch/nuc140/regs.h"
#include "../arch/nuc140/arch.h"
#include "../kernel/sync.h"
#include "../kernel/task.h"

static struct mutex device;
void ch376_lock(void) { mutex_lock(&device); }
int ch376_trylock(void) { return mutex_trylock(&device); }
void ch376_unlock(void) { mutex_unlock(&device); }

#define INT_ACTIVE() (!(GPIO_PIN(PORTB) & BIT(14)))

static uint32_t commands;            /* every command sent: a listing in progress is gone */
uint32_t ch376_commands(void) { return commands; }

static void begin(uint8_t cmd)
{
    ++commands;
    spi_select(SPI_CH376);
    spi_xfer8(cmd);
    delay_us(2);                     /* >= 1.5 us between command and data */
}
static void end(void) { spi_select(SPI_NONE); delay_us(1); }

static uint8_t cmd_read(uint8_t cmd)
{
    begin(cmd);
    uint8_t v = spi_xfer8(0);
    end();
    return v;
}

/* INT's falling edge (board_ch376_irq) gives `done`. Commands answer within
 * microseconds or take milliseconds (disk I/O); for the long ones the caller
 * (usually fsd, priority 4) sleeps instead of spinning, so lower-priority
 * tasks such as a player keep running. The level is what counts: the
 * semaphore only wakes the wait, and the timeout re-checks every 10 ms in
 * case an edge was missed. Before the scheduler runs, it spins. */
static struct sem done;
void ch376_isr(void) { sem_give(&done); }

static uint8_t wait_status(uint32_t ms)
{
    uint32_t s = irq_save();
    done.count = 0;                  /* edges of earlier commands */
    irq_restore(s);
    for (int i = 0; i < 50 && !INT_ACTIVE(); ++i) delay_us(1);
    uint32_t start = jiffies;
    while (!INT_ACTIVE()) {
        uint32_t waited = jiffies - start;
        if (waited > ms) return CH376_TIMEOUT;
        if (sched_running()) sem_take(&done, ms - waited < 10u ? ms - waited + 1u : 10u);
    }
    return cmd_read(0x22);           /* GET_STATUS, clears INT */
}

static uint8_t op(uint8_t cmd, const uint8_t *data, unsigned n)
{
    begin(cmd);
    for (unsigned i = 0; i < n; ++i) spi_xfer8(data[i]);
    end();
    return wait_status(5000);
}

static int set_mode(uint8_t mode)
{
    begin(0x15);
    spi_xfer8(mode);
    delay_us(20);
    uint8_t r = spi_xfer8(0);
    end();
    return r == 0x51;
}

static void set_name(const char *path)
{
    begin(0x2f);
    while (*path) spi_xfer8((uint8_t)*path++);
    spi_xfer8(0);
    end();
}

int ch376_present(void)
{
    begin(0x06);
    spi_xfer8(0x5a);
    uint8_t v = spi_xfer8(0);
    end();
    return v == 0xa5;
}

uint8_t ch376_version(void) { return cmd_read(0x01); }

int ch376_mount(void)
{
    begin(0x05); end();              /* RESET_ALL */
    msleep(100);
    if (!set_mode(5)) return 1;
    if (wait_status(2000) != 0x15) return 2;   /* no disk attached */
    set_mode(7); msleep(50);
    set_mode(6); msleep(50);
    for (int i = 0; i < 8 && INT_ACTIVE(); ++i) cmd_read(0x22);
    if (op(0x30, 0, 0) != CH376_OK) return 3;  /* DISK_CONNECT */
    for (int i = 0; i < 5; ++i) {
        if (op(0x31, 0, 0) == CH376_OK) return 0;  /* DISK_MOUNT */
        msleep(200);
    }
    return 4;
}

uint8_t ch376_open(const char *path) { set_name(path); return op(0x32, 0, 0); }
uint8_t ch376_create(const char *path) { set_name(path); return op(0x34, 0, 0); }
uint8_t ch376_erase(const char *path) { set_name(path); return op(0x35, 0, 0); }

uint8_t ch376_close(int update_size)
{
    uint8_t arg = update_size ? 1 : 0;
    return op(0x36, &arg, 1);
}

uint32_t ch376_file_size(void)
{
    uint32_t size = 0;
    begin(0x0c);
    spi_xfer8(0x68);
    delay_us(2);
    for (int i = 0; i < 4; ++i) size |= (uint32_t)spi_xfer8(0) << (8 * i);
    end();
    return size;
}

uint8_t ch376_write(const void *buf, uint32_t len)
{
    const uint8_t *p = buf;
    while (len) {
        uint32_t chunk = len > 0x8000u ? 0x8000u : len, done = 0;
        uint8_t arg[2] = { (uint8_t)chunk, (uint8_t)(chunk >> 8) };
        uint8_t s = op(0x3c, arg, 2);                  /* BYTE_WRITE */
        while (s == 0x1e) {
            begin(0x2d);                               /* WR_REQ_DATA */
            uint32_t n = spi_xfer8(0);
            if (n > chunk - done) n = chunk - done;
            for (uint32_t i = 0; i < n; ++i) spi_xfer8(p[done + i]);
            end();
            done += n;
            s = op(0x3d, 0, 0);                        /* BYTE_WR_GO */
        }
        if (s != CH376_OK) return s;
        if (done != chunk) return 0xfe;
        p += chunk;
        len -= chunk;
    }
    return CH376_OK;
}

uint8_t ch376_read(void *buf, uint32_t len, uint32_t *got)
{
    uint8_t *p = buf;
    *got = 0;
    while (len) {
        uint32_t chunk = len > 0x8000u ? 0x8000u : len, done = 0;
        uint8_t arg[2] = { (uint8_t)chunk, (uint8_t)(chunk >> 8) };
        uint8_t s = op(0x3a, arg, 2);                  /* BYTE_READ */
        while (s == 0x1d) {
            begin(0x27);                               /* RD_USB_DATA0 */
            uint32_t n = spi_xfer8(0);
            for (uint32_t i = 0; i < n; ++i) {
                uint8_t v = spi_xfer8(0);
                if (done + i < chunk) p[done + i] = v;
            }
            end();
            if (n > chunk - done) return 0xfd;
            done += n;
            s = op(0x3b, 0, 0);                        /* BYTE_RD_GO */
        }
        if (s != CH376_OK) return s;
        *got += done;
        if (done < chunk) return CH376_OK;             /* end of file */
        p += chunk;
        len -= chunk;
    }
    return CH376_OK;
}

uint8_t ch376_byte_locate(uint32_t offset)
{
    uint8_t arg[4] = { (uint8_t)offset, (uint8_t)(offset >> 8), (uint8_t)(offset >> 16), (uint8_t)(offset >> 24) };
    return op(0x39, arg, 4);                           /* BYTE_LOCATE */
}

uint8_t ch376_dir_create(const char *name) { set_name(name); return op(0x40, 0, 0); }

/* FILE_ERASE removes whatever is currently open: open the target itself first.
 * (Opening only its parent directory and naming the target removes the parent.) */
uint8_t ch376_erase_open(void) { return op(0x35, 0, 0); }

/* Directory listing: open the directory first, then ch376_enum_first("*"). Each
 * CH376_ENTRY_READY status leaves one 32-byte FAT entry to fetch with
 * ch376_read_entry(); ch376_enum_next() moves on until CH376_MISS_FILE. */
uint8_t ch376_enum_first(const char *pattern) { set_name(pattern); return op(0x32, 0, 0); }
uint8_t ch376_enum_next(void) { return op(0x33, 0, 0); }

int ch376_read_entry(uint8_t entry[32])
{
    begin(0x27);                                       /* RD_USB_DATA0 */
    uint32_t n = spi_xfer8(0);
    for (uint32_t i = 0; i < n; ++i) {
        uint8_t v = spi_xfer8(0);
        if (i < 32) entry[i] = v;
    }
    end();
    return (int)n;
}

uint8_t ch376_disk_query(uint32_t *total_sectors, uint32_t *free_sectors)
{
    uint8_t s = op(0x3f, 0, 0);                        /* DISK_QUERY */
    if (s != CH376_OK) return s;
    uint8_t d[9] = { 0 };
    begin(0x27);
    uint32_t n = spi_xfer8(0);
    for (uint32_t i = 0; i < n; ++i) { uint8_t v = spi_xfer8(0); if (i < sizeof d) d[i] = v; }
    end();
    *total_sectors = d[0] | (uint32_t)d[1] << 8 | (uint32_t)d[2] << 16 | (uint32_t)d[3] << 24;
    *free_sectors = d[4] | (uint32_t)d[5] << 8 | (uint32_t)d[6] << 16 | (uint32_t)d[7] << 24;
    return CH376_OK;
}
