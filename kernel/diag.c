/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdarg.h>
#include "diag.h"
#include "kernel.h"
#include "klog.h"
#include "kmem.h"
#include "modules.h"
#include "task.h"
#include "../lib/fmt.h"
#include "../arch/nuc140/arch.h"
#include "../drivers/spi.h"
#include "../drivers/uart.h"
#include "../drivers/ch376.h"
#include "../drivers/w5500.h"
#include "../user/demo.h"

#define SOCK 0
#define PORT 5000
#define CHUNK 1024u

static uint8_t buf[CHUNK];           /* network transfers of the diag task */
static char reply[384];
static unsigned reply_len;
static struct task *reply_owner;     /* only the diag task appends to the TCP reply */

static uint8_t pattern(uint32_t i) { return (uint8_t)(((i * 0x9e3779b1u) >> 24) ^ i); }

/* Append to the TCP reply (diag task only) and echo to the kernel log. */
static void report(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void report(const char *fmt, ...)
{
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    int n = nn_vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof line - 1) n = sizeof line - 1;
    if (task_self() == reply_owner)
        for (int i = 0; i < n && reply_len < sizeof reply; ++i) reply[reply_len++] = line[i];
    kprintf("%s", line);
}

static void listen(void)
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

static void ch376_bench_locked(uint32_t size, uint8_t *data)
{
    char name[] = "/B376xxxx.BIN";
    uint32_t errors = 0, got = 0;
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
        for (uint32_t k = 0; k < m; ++k) data[k] = pattern(pos + k);
        s = ch376_write(data, m);
    }
    uint8_t c = ch376_close(1);
    uint32_t t1 = jiffies;
    if (s != CH376_OK || c != CH376_OK) { report(" write 0x%02x/0x%02x FAIL\n", s, c); return; }
    s = ch376_open(name);
    if (s != CH376_OK) { report(" reopen 0x%02x FAIL\n", s); return; }
    uint32_t stored = ch376_file_size(), t2 = jiffies;
    for (uint32_t pos = 0; pos < size && s == CH376_OK; pos += CHUNK) {
        uint32_t m = size - pos < CHUNK ? size - pos : CHUNK, part;
        s = ch376_read(data, m, &part);
        for (uint32_t k = 0; k < part; ++k) if (data[k] != pattern(pos + k)) errors++;
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

void diag_ch376_bench(uint32_t size)
{
    uint8_t *data = kmalloc(CHUNK);
    report("ch376 bench %u B spi div %u:", size, spi_divider(SPI_CH376));
    if (!data) { report(" out of memory FAIL\n"); return; }
    ch376_lock();
    ch376_bench_locked(size, data);
    ch376_unlock();
    kfree(data);
}

static const char *const state_names[] = { "-", "run", "sleep", "wait", "zombie" };

static void ps_text(void)
{
    struct task_info info[TASK_MAX];
    struct kmem_stats m;
    int n = task_list(info, TASK_MAX);
    report("  pid name     pri state  stack used/size  cpu ms\n");
    for (int i = 0; i < n; ++i)
        report("%5u %-8s %3u %-6s %5u/%-5u %9u\n", info[i].pid, info[i].name, info[i].prio,
               state_names[info[i].state], info[i].stack_used, info[i].stack_size, info[i].cpu_ms);
    kmem_stats(&m);
    report("heap %u/%u B free, largest %u; switches %u; msp %u/%u B\n",
           m.free, m.total, m.largest, sched_switches(), stack_used(), stack_size());
}

static void status_text(void)
{
    struct uart1_stats u;
    struct kmem_stats m;
    uart1_stats(&u);
    kmem_stats(&m);
    report("nnnix %s uptime %u ms\n", nnnix_version, jiffies);
    report("reset 0x%02x msp %u/%u B static ram %u B heap free %u B\n",
           board_reset_cause(), stack_used(), stack_size(), ram_static(), m.free);
    report("modules power %u ch376 %u w5500 %u link 0x%x\n", modules.powered, modules.ch376, modules.w5500, modules.link);
    report("uart1 %u B err %u drop %u spi timeouts %u\n", u.bytes, u.errors, u.drops, spi_timeouts());
}

static void demo(uint32_t ms)
{
    if (ms < 100 || ms > 10000) ms = 1000;
    uint32_t until = jiffies + ms, switches0 = sched_switches();
    struct task *a = task_spawn("hog_a", demo_hog, (void *)(uintptr_t)until, 512, 5, 0);
    struct task *b = task_spawn("hog_b", demo_hog, (void *)(uintptr_t)until, 512, 5, 0);
    struct task *t = task_spawn("ticker", demo_ticker, (void *)(uintptr_t)(ms / 10u), 512, 4, 0);
    if (!a || !b || !t) { report("demo: spawn failed\n"); return; }
    uint32_t cpu_t, cpu_a, cpu_b;
    int late = task_join(t, &cpu_t);
    int loops_a = task_join(a, &cpu_a), loops_b = task_join(b, &cpu_b);
    report("demo %u ms: hog_a cpu %u ms loops %d, hog_b cpu %u ms loops %d\n",
           ms, cpu_a, loops_a, cpu_b, loops_b);
    report("demo ticker: worst late %d ms, cpu %u ms; context switches %u\n",
           late, cpu_t, sched_switches() - switches0);
}

static void session(void)
{
    uint8_t h[6];
    if (w5500_recv(SOCK, h, 6) != 6) return;
    uint32_t length = ((uint32_t)h[2] << 24) | ((uint32_t)h[3] << 16) | ((uint32_t)h[4] << 8) | h[5];
    unsigned div = h[1] <= 63u ? h[1] : SPI_DEFAULT_DIVIDER;
    reply_len = 0;
    reply_owner = task_self();
    switch (h[0]) {
    case 'P':
        report("nnnix %s diag hclk %u\n", nnnix_version, HCLK_HZ);
        break;
    case 'S':
        status_text();
        break;
    case 'K':
        ps_text();
        break;
    case 'L': {
        uint16_t n = (uint16_t)klog_read((char *)buf, sizeof buf);
        w5500_send(SOCK, buf, n, 2000);
        break;
    }
    case 'T': {
        spi_set_divider(SPI_W5500, div);
        uint32_t t0 = jiffies;
        int r = tx_pattern(length);
        kprintf("diag: tx %u B div %u %u ms%s\n", length, div, jiffies - t0, r ? " ABORT" : "");
        break;
    }
    case 'R': {
        spi_set_divider(SPI_W5500, div);
        uint32_t errors = 0, t0 = jiffies;
        int r = rx_pattern(length, &errors);
        uint32_t ms = jiffies - t0, out[3] = { length, r ? 0xffffffffu : errors, ms };
        for (int i = 0; i < 3; ++i)
            for (int b = 0; b < 4; ++b) reply[reply_len++] = (char)(out[i] >> (24 - 8 * b));
        kprintf("diag: rx %u B div %u %u ms errors %u%s\n", length, div, ms, errors, r ? " ABORT" : "");
        break;
    }
    case 'U':
        spi_set_divider(SPI_CH376, div);
        diag_ch376_bench(length);
        break;
    case 'D':
        demo(length);
        break;
    case 'O':
        report(task_spawn("ovf", demo_overflow, 0, 512, 4, 1) ? "overflow task started\n" : "spawn failed\n");
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
    reply_owner = 0;
    if (reply_len) w5500_send(SOCK, reply, (uint16_t)reply_len, 2000);
    w5500_flush(SOCK, 2000);
    spi_set_divider(SPI_W5500, SPI_DEFAULT_DIVIDER);
    spi_set_divider(SPI_CH376, SPI_DEFAULT_DIVIDER);
    w5500_disconnect(SOCK, 1000);
    listen();
}

static void poll_socket(void)
{
    static uint8_t last_sr;
    static uint32_t since;
    uint8_t sr = w5500_status(SOCK);
    if (sr != last_sr) { last_sr = sr; since = jiffies; }
    if (sr == W5500_SOCK_SYNRECV && jiffies - since > 2000u) {
        /* A SYN that arrived while the peer's link was still coming up leaves the
         * only listening socket in SYN_RECV for ~32 s (measured after reboots). */
        kprintf("diag: dropping stale half-open connection\n");
        listen();
        return;
    }
    if (sr == W5500_SOCK_ESTABLISHED) {
        if (w5500_rx_available(SOCK) >= 6) session();
    } else if (sr == W5500_SOCK_CLOSE_WAIT) {
        w5500_disconnect(SOCK, 500);
        listen();
    } else if (sr == W5500_SOCK_CLOSED) {
        listen();
    }
}

void diag_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (modules.w5500) poll_socket();
        task_sleep(modules.w5500 ? 1 : 100);
    }
}
