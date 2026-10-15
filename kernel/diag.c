#include <stdarg.h>
#include "diag.h"
#include "kernel.h"
#include "klog.h"
#include "modules.h"
#include "../lib/fmt.h"
#include "../arch/nuc140/arch.h"
#include "../drivers/spi.h"
#include "../drivers/uart.h"
#include "../drivers/ch376.h"
#include "../drivers/w5500.h"

#define SOCK 0
#define PORT 5000
#define CHUNK 1024u

static uint8_t buf[CHUNK];
static char reply[384];
static unsigned reply_len;

static uint8_t pattern(uint32_t i) { return (uint8_t)(((i * 0x9e3779b1u) >> 24) ^ i); }

/* Append to the TCP reply and echo to the kernel log. */
static void report(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void report(const char *fmt, ...)
{
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    int n = nn_vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof line - 1) n = sizeof line - 1;
    for (int i = 0; i < n && reply_len < sizeof reply; ++i) reply[reply_len++] = line[i];
    kprintf("%s", line);
}

void diag_start(void)
{
    if (w5500_tcp_listen(SOCK, PORT) == 0) kprintf("diag: listening on tcp port %u\n", PORT);
    else kprintf("diag: listen failed\n");
}

static int tx_pattern(uint32_t length)
{
    uint32_t sent = 0, queued = 0, last = jiffies;
    while (sent < length) {
        uint32_t n = length - sent < CHUNK ? length - sent : CHUNK;
        if (w5500_tx_room(SOCK) < n) {
            if (queued) { if (w5500_push(SOCK)) return -1; queued = 0; }
            if (w5500_status(SOCK) != W5500_SOCK_ESTABLISHED || jiffies - last > 5000u) return -1;
            continue;
        }
        for (uint32_t k = 0; k < n; ++k) buf[k] = pattern(sent + k);
        if (w5500_write(SOCK, buf, (uint16_t)n)) return -1;
        sent += n; queued += n; last = jiffies;
        if (queued >= 8192u) { if (w5500_push(SOCK)) return -1; queued = 0; }
    }
    if (queued && w5500_push(SOCK)) return -1;
    return w5500_flush(SOCK, 5000);
}

static int rx_pattern(uint32_t length, uint32_t *errors)
{
    uint32_t received = 0, last = jiffies;
    while (received < length) {
        uint32_t want = length - received < CHUNK ? length - received : CHUNK;
        uint16_t n = w5500_recv(SOCK, buf, (uint16_t)want);
        if (!n) {
            uint8_t sr = w5500_status(SOCK);
            if ((sr != W5500_SOCK_ESTABLISHED && sr != W5500_SOCK_CLOSE_WAIT) || jiffies - last > 5000u) return -1;
            continue;
        }
        for (uint32_t k = 0; k < n; ++k) if (buf[k] != pattern(received + k)) (*errors)++;
        received += n; last = jiffies;
    }
    return 0;
}

static void rate(const char *label, uint32_t bytes, uint32_t ms)
{
    report("%s %u ms %u kB/s", label, ms, ms ? bytes / ms : 0);   /* bytes/ms == kB/s */
}

void diag_ch376_bench(unsigned long length)
{
    static char name[] = "/B376xxxx.BIN";
    uint32_t size = (uint32_t)length, errors = 0, got = 0;
    report("ch376 bench %u B spi div %u:", size, spi_divider());
    int e = ch376_mount();
    if (e) { report(" mount failed (%d) FAIL\n", e); return; }
    unsigned n;
    for (n = 0; n < 10000; ++n) {
        nn_snprintf(name + 5, 9, "%04u", n);
        name[9] = '.';
        if (ch376_open(name) == CH376_MISS_FILE) break;
        ch376_close(0);
    }
    if (n == 10000) { report(" no free file name FAIL\n"); return; }
    uint8_t s = ch376_create(name);
    if (s != CH376_OK) { report(" create 0x%02x FAIL\n", s); return; }
    uint32_t t0 = jiffies;
    for (uint32_t pos = 0; pos < size && s == CH376_OK; pos += CHUNK) {
        uint32_t m = size - pos < CHUNK ? size - pos : CHUNK;
        for (uint32_t k = 0; k < m; ++k) buf[k] = pattern(pos + k);
        s = ch376_write(buf, m);
    }
    uint8_t c = ch376_close(1);
    uint32_t t1 = jiffies;
    if (s != CH376_OK || c != CH376_OK) { report(" write 0x%02x/0x%02x FAIL\n", s, c); return; }
    s = ch376_open(name);
    if (s != CH376_OK) { report(" reopen 0x%02x FAIL\n", s); return; }
    uint32_t stored = ch376_file_size(), t2 = jiffies;
    for (uint32_t pos = 0; pos < size && s == CH376_OK; pos += CHUNK) {
        uint32_t m = size - pos < CHUNK ? size - pos : CHUNK, part;
        s = ch376_read(buf, m, &part);
        for (uint32_t k = 0; k < part; ++k) if (buf[k] != pattern(pos + k)) errors++;
        got += part;
        if (part < m) break;
    }
    ch376_close(0);
    uint32_t t3 = jiffies;
    uint8_t erased = ch376_erase(name);
    report(" %s", name + 1);
    rate(" write", size, t1 - t0);
    rate(", read", size, t3 - t2);
    report(", size %u, read %u, errors %u, %s %s\n", stored, got, errors,
           erased == CH376_OK ? "deleted" : "delete failed",
           (s == CH376_OK && !errors && stored == size && got == size) ? "OK" : "FAIL");
}

static void status_text(void)
{
    struct uart1_stats u;
    uart1_stats(&u);
    report("nnnix %s uptime %u ms\n", nnnix_version, jiffies);
    report("reset 0x%02x stack %u/%u B static ram %u B\n", board_reset_cause(), stack_used(), stack_size(), ram_static());
    report("modules power %u ch376 %u w5500 %u link 0x%x\n", modules.powered, modules.ch376, modules.w5500, modules.link);
    report("uart1 %u B err %u drop %u spi timeouts %u\n", u.bytes, u.errors, u.drops, spi_timeouts());
}

static void session(void)
{
    uint8_t h[6];
    if (w5500_recv(SOCK, h, 6) != 6) return;
    uint32_t length = ((uint32_t)h[2] << 24) | ((uint32_t)h[3] << 16) | ((uint32_t)h[4] << 8) | h[5];
    unsigned div = h[1] <= 63u ? h[1] : SPI_DEFAULT_DIVIDER;
    reply_len = 0;
    spi_set_divider(div);
    switch (h[0]) {
    case 'P':
        report("nnnix %s diag hclk %u spi div %u\n", nnnix_version, HCLK_HZ, div);
        break;
    case 'S':
        status_text();
        break;
    case 'L': {
        uint16_t n = (uint16_t)klog_read((char *)buf, sizeof buf);
        w5500_send(SOCK, buf, n, 2000);
        break;
    }
    case 'T': {
        uint32_t t0 = jiffies;
        int r = tx_pattern(length);
        kprintf("diag: tx %u B div %u %u ms%s\n", length, div, jiffies - t0, r ? " ABORT" : "");
        break;
    }
    case 'R': {
        uint32_t errors = 0, t0 = jiffies;
        int r = rx_pattern(length, &errors);
        uint32_t ms = jiffies - t0, out[3] = { length, r ? 0xffffffffu : errors, ms };
        for (int i = 0; i < 3; ++i)
            for (int b = 0; b < 4; ++b) reply[reply_len++] = (char)(out[i] >> (24 - 8 * b));
        kprintf("diag: rx %u B div %u %u ms errors %u%s\n", length, div, ms, errors, r ? " ABORT" : "");
        break;
    }
    case 'U':
        diag_ch376_bench(length);
        break;
    case 'F':
    case 'X':
        report(h[0] == 'F' ? "fault injection\n" : "rebooting\n");
        w5500_send(SOCK, reply, (uint16_t)reply_len, 1000);
        w5500_flush(SOCK, 1000);
        w5500_disconnect(SOCK, 500);
        if (h[0] == 'F') fault_trigger();
        arch_reboot();
    default:
        report("unknown command 0x%02x\n", h[0]);
        break;
    }
    if (reply_len) w5500_send(SOCK, reply, (uint16_t)reply_len, 2000);
    w5500_flush(SOCK, 2000);
    spi_set_divider(SPI_DEFAULT_DIVIDER);
    w5500_disconnect(SOCK, 1000);
    diag_start();
}

void diag_poll(void)
{
    static uint8_t last_sr;
    static uint32_t since;
    uint8_t sr = w5500_status(SOCK);
    if (sr != last_sr) { last_sr = sr; since = jiffies; }
    if (sr == W5500_SOCK_SYNRECV && jiffies - since > 2000u) {
        /* A SYN that arrived while the peer's link was still coming up leaves the
         * only listening socket in SYN_RECV for ~32 s (measured after reboots). */
        kprintf("diag: dropping stale half-open connection\n");
        diag_start();
        return;
    }
    if (sr == W5500_SOCK_ESTABLISHED) {
        if (w5500_rx_available(SOCK) >= 6) session();
    } else if (sr == W5500_SOCK_CLOSE_WAIT) {
        w5500_disconnect(SOCK, 500);
        diag_start();
    } else if (sr == W5500_SOCK_CLOSED) {
        diag_start();
    }
}
