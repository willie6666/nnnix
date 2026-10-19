/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "w5500.h"
#include "spi.h"
#include "../arch/nuc140/arch.h"
#include "../kernel/sync.h"
#include "../kernel/task.h"

/* Block select: 0 common, socket n: registers 1+4n, TX buffer 2+4n, RX buffer 3+4n. */
#define SREG(sn) ((uint8_t)(1u + 4u * (unsigned)(sn)))
#define STX(sn)  ((uint8_t)(2u + 4u * (unsigned)(sn)))
#define SRX(sn)  ((uint8_t)(3u + 4u * (unsigned)(sn)))
enum { Sn_MR = 0x00, Sn_CR = 0x01, Sn_IR = 0x02, Sn_SR = 0x03, Sn_PORT = 0x04,
       Sn_RXBUF = 0x1e, Sn_TXBUF = 0x1f, Sn_TX_FSR = 0x20, Sn_TX_WR = 0x24,
       Sn_TX_RD = 0x22, Sn_RX_RSR = 0x26, Sn_RX_RD = 0x28, Sn_IMR = 0x2c };
enum { SIR = 0x0017, SIMR = 0x0018 };
enum { CR_OPEN = 0x01, CR_LISTEN = 0x02, CR_CONNECT = 0x04, CR_DISCON = 0x08, CR_CLOSE = 0x10,
       CR_SEND = 0x20, CR_RECV = 0x40 };
enum { IR_TIMEOUT = 0x08, IR_SEND_OK = 0x10 };

static uint16_t tx_size[8];
static uint16_t queued[8];           /* bytes written since the last SEND */
/* A SEND awaits SEND_OK. One byte per socket, not a shared bit mask: tasks on
 * different sockets update theirs concurrently, and a read-modify-write of a
 * shared mask lost updates (a stuck flag made every later send wait 5 s; a lost
 * one let a SEND overtake the previous one, which drops data). */
static volatile uint8_t send_pending[8];
static uint32_t missed_send_ok;     /* SENDs completed without a SEND_OK (statistics) */
static uint8_t events[8];            /* Sn_IR bits read (and cleared on the chip) but not consumed */
static void (*notify)(void);         /* INTn fell */
static struct sem sock_ev[8];        /* something happened on socket n */

static void header(uint16_t address, uint8_t control)
{
    spi_select(SPI_W5500);
    spi_xfer8((uint8_t)(address >> 8));
    spi_xfer8((uint8_t)address);
    spi_xfer8(control);
}
/* Register and buffer access: 0, or -1 when the SPI transfer failed (spi.h).
 * A failed read gives zeros, which the chip's registers read as a closed
 * socket with nothing received and no room to send: callers that only look
 * at values fail safe, and the data paths below check. */
static int wr(uint16_t address, uint8_t block, const void *p, uint32_t n)
{
    header(address, (uint8_t)((block << 3) | 4u));
    spi_write(p, n);
    return spi_end();
}
static int rd(uint16_t address, uint8_t block, void *p, uint32_t n)
{
    header(address, (uint8_t)(block << 3));
    spi_read(p, n);
    return spi_end();
}
static uint8_t r8(uint16_t a, uint8_t b) { uint8_t v; rd(a, b, &v, 1); return v; }
static int w8(uint16_t a, uint8_t b, uint8_t v) { return wr(a, b, &v, 1); }
static uint16_t r16(uint16_t a, uint8_t b) { uint8_t v[2]; rd(a, b, v, 2); return (uint16_t)((v[0] << 8) | v[1]); }
static int r16_checked(uint16_t a, uint8_t b, uint16_t *v)
{
    uint8_t x[2];
    int e = rd(a, b, x, 2);
    *v = (uint16_t)((x[0] << 8) | x[1]);
    return e;
}
static int w16(uint16_t a, uint8_t b, uint16_t v) { uint8_t x[2] = { (uint8_t)(v >> 8), (uint8_t)v }; return wr(a, b, x, 2); }
static uint16_t r16_stable(uint16_t a, uint8_t b)
{
    /* FSR/RSR may change between the two byte reads. */
    uint16_t x = r16(a, b), y;
    while ((y = r16(a, b)) != x) x = y;
    return x;
}
/* 0 once the chip has taken the command (Sn_CR reads 0 again), -1 if the
 * write failed or it did not within 100 ms. */
static int command(int sn, uint8_t cmd)
{
    if (w8(Sn_CR, SREG(sn), cmd)) return -1;
    uint32_t start = jiffies;
    while (r8(Sn_CR, SREG(sn))) if (jiffies - start >= 100u) return -1;
    return 0;
}

uint32_t w5500_missed_send_ok(void) { return missed_send_ok; }

int w5500_present(void) { return r8(0x0039, 0) == 0x04; }

/* ---- interrupts: INTn -> w5500_isr -> netd -> w5500_collect -> per-socket wakeups ---- */
void w5500_isr(void) { if (notify) notify(); }
void w5500_set_notify(void (*fn)(void)) { notify = fn; }

static uint8_t read_events(int sn)
{
    uint8_t v = r8(Sn_IR, SREG(sn));
    if (v) w8(Sn_IR, SREG(sn), v);                  /* acknowledge on the chip */
    uint32_t s = irq_save();
    events[sn] |= v;
    v = events[sn];
    irq_restore(s);
    return v;
}

static void consume(int sn, uint8_t bits)
{
    uint32_t s = irq_save();
    events[sn] &= (uint8_t)~bits;
    irq_restore(s);
}

static void wake(int sn) { if (sock_ev[sn].count <= 0) sem_give(&sock_ev[sn]); }

uint8_t w5500_collect(void)
{
    uint8_t all = 0;
    for (int round = 0; round < 4; ++round) {       /* INTn stays low until SIR reads 0 */
        uint8_t sir = r8(SIR, 0);
        if (!sir) break;
        all |= sir;
        for (int sn = 0; sn < 8; ++sn)
            if (sir & (1u << sn)) { read_events(sn); wake(sn); }
    }
    return all;
}

int w5500_wait(int sn, uint32_t timeout_ms) { return sem_take(&sock_ev[sn], timeout_ms); }
uint8_t w5500_phy(void) { return r8(0x002e, 0); }

void w5500_set_addr(const struct w5500_netcfg *cfg)
{
    uint8_t common[18];
    for (int i = 0; i < 4; ++i) common[i] = cfg->gateway[i];
    for (int i = 0; i < 4; ++i) common[4 + i] = cfg->mask[i];
    for (int i = 0; i < 6; ++i) common[8 + i] = cfg->mac[i];
    for (int i = 0; i < 4; ++i) common[14 + i] = cfg->ip[i];
    wr(0x0001, 0, common, sizeof common);          /* GAR SUBR SHAR SIPR are contiguous */
}

void w5500_configure(const struct w5500_netcfg *cfg, const uint8_t buffer_kib[8])
{
    w5500_set_addr(cfg);
    for (int sn = 0; sn < 8; ++sn) {
        w8(Sn_RXBUF, SREG(sn), buffer_kib[sn]);
        w8(Sn_TXBUF, SREG(sn), buffer_kib[sn]);
        tx_size[sn] = (uint16_t)(buffer_kib[sn] * 1024u);
        queued[sn] = 0;
        events[sn] = 0;
        w8(Sn_IMR, SREG(sn), buffer_kib[sn] ? 0x1f : 0);   /* SEND_OK TIMEOUT RECV DISCON CON */
    }
    uint8_t simr = 0;
    for (int sn = 0; sn < 8; ++sn) if (buffer_kib[sn]) simr |= (uint8_t)(1u << sn);
    w8(SIMR, 0, simr);
    for (int sn = 0; sn < 8; ++sn) send_pending[sn] = 0;
}

static void reset_state(int sn)
{
    w8(Sn_IR, SREG(sn), 0x1f);
    send_pending[sn] = 0;
    queued[sn] = 0;
    consume(sn, 0xff);
}

int w5500_open(int sn, uint8_t mode, uint16_t port)
{
    command(sn, CR_CLOSE);
    w8(Sn_MR, SREG(sn), mode);
    w16(Sn_PORT, SREG(sn), port);
    reset_state(sn);
    command(sn, CR_OPEN);
    uint8_t want = (mode & 0x0fu) == W5500_MODE_UDP ? W5500_SOCK_UDP : W5500_SOCK_INIT;
    return r8(Sn_SR, SREG(sn)) == want ? 0 : -1;
}

void w5500_close(int sn)
{
    command(sn, CR_CLOSE);
    reset_state(sn);
}

int w5500_tcp_listen(int sn, uint16_t port)
{
    if (w5500_open(sn, W5500_MODE_TCP_ND, port)) return -1;
    command(sn, CR_LISTEN);
    return r8(Sn_SR, SREG(sn)) == W5500_SOCK_LISTEN ? 0 : -1;
}

void w5500_set_peer(int sn, const uint8_t ip[4], uint16_t port)
{
    wr(0x0c, SREG(sn), ip, 4);                      /* Sn_DIPR */
    w16(0x10, SREG(sn), port);                      /* Sn_DPORT */
}

int w5500_tcp_connect(int sn, const uint8_t ip[4], uint16_t port, uint16_t local_port)
{
    if (w5500_open(sn, W5500_MODE_TCP_ND, local_port)) return -1;
    w5500_set_peer(sn, ip, port);
    command(sn, CR_CONNECT);
    return 0;
}

uint8_t w5500_mode(int sn) { return r8(Sn_MR, SREG(sn)); }
uint16_t w5500_port(int sn) { return r16(Sn_PORT, SREG(sn)); }

uint8_t w5500_status(int sn) { return r8(Sn_SR, SREG(sn)); }
int w5500_can_send(int sn)
{
    uint8_t sr = w5500_status(sn);
    return sr == W5500_SOCK_ESTABLISHED || sr == W5500_SOCK_CLOSE_WAIT || sr == W5500_SOCK_UDP;
}

void w5500_peer(int sn, uint8_t ip[4], uint16_t *port)
{
    rd(0x0c, SREG(sn), ip, 4);                      /* Sn_DIPR */
    *port = r16(0x10, SREG(sn));                    /* Sn_DPORT */
}
uint16_t w5500_rx_available(int sn) { return r16_stable(Sn_RX_RSR, SREG(sn)); }
uint16_t w5500_tx_free(int sn) { return r16_stable(Sn_TX_FSR, SREG(sn)); }

/* A failed transfer takes nothing: the data stays for the next call. */
int w5500_recv(int sn, void *buf, uint16_t n)
{
    uint16_t avail = w5500_rx_available(sn), ptr;
    if (n > avail) n = avail;
    if (!n) return 0;
    /* A failure from here on leaves the chip's read pointer unknown: the
     * caller must not take what follows as the next bytes (review R50). */
    if (r16_checked(Sn_RX_RD, SREG(sn), &ptr) ||
        rd(ptr, SRX(sn), buf, n) ||                 /* chip wraps the offset itself */
        w16(Sn_RX_RD, SREG(sn), (uint16_t)(ptr + n)) || command(sn, CR_RECV)) return -1;
    return n;
}

int w5500_skip(int sn, uint16_t n)
{
    uint16_t avail = w5500_rx_available(sn), ptr;
    if (n > avail) n = avail;
    if (!n) return 0;
    if (r16_checked(Sn_RX_RD, SREG(sn), &ptr) || w16(Sn_RX_RD, SREG(sn), (uint16_t)(ptr + n)) ||
        command(sn, CR_RECV)) return -1;
    return 0;
}

int w5500_write(int sn, const void *buf, uint16_t n)
{
    /* Sn_TX_WR and Sn_TX_FSR read back their values as of the last SEND (measured),
     * so data queued since then is tracked here; otherwise each write overwrites
     * the previous one at the same offset. */
    uint16_t ptr;
    if ((uint32_t)n + queued[sn] > w5500_tx_free(sn) || r16_checked(Sn_TX_WR, SREG(sn), &ptr)) return -1;
    ptr = (uint16_t)(ptr + queued[sn]);
    if (wr(ptr, STX(sn), buf, n) || w16(Sn_TX_WR, SREG(sn), (uint16_t)(ptr + n))) return -1;
    queued[sn] = (uint16_t)(queued[sn] + n);
    return 0;
}

uint16_t w5500_tx_room(int sn)
{
    uint16_t free = w5500_tx_free(sn);
    return free > queued[sn] ? (uint16_t)(free - queued[sn]) : 0;
}

static int wait_send_ok(int sn, uint32_t timeout_ms)
{
    if (!send_pending[sn]) return 0;
    uint32_t start = jiffies;
    for (;;) {
        uint8_t ir = read_events(sn);
        if (ir & IR_SEND_OK) {
            consume(sn, IR_SEND_OK);
            send_pending[sn] = 0;
            return 0;
        }
        /* A FIN closes only the peer's sending direction. W5500 permits SEND
         * in CLOSE_WAIT; keep waiting for SEND_OK so the reply reaches it. */
        if (ir & IR_TIMEOUT) {                      /* TCP gave up, or UDP found no ARP answer */
            consume(sn, IR_TIMEOUT);
            send_pending[sn] = 0;
            return -1;
        }
        if (!w5500_can_send(sn) || jiffies - start > timeout_ms) return -1;
        /* Measured (a new telnet session, about 1 in 20): SEND_OK never came
         * although everything was sent and acknowledged; likely an earlier
         * SEND took data queued just after it, leaving a SEND with nothing to
         * do. An empty buffer with the read pointer at the write pointer means
         * the SEND is complete either way. */
        if (jiffies - start > 20u && w5500_tx_free(sn) == tx_size[sn] &&
            r16(Sn_TX_RD, SREG(sn)) == r16(Sn_TX_WR, SREG(sn))) {
            send_pending[sn] = 0;
            missed_send_ok++;
            return 0;
        }
        w5500_wait(sn, 1);
    }
}

/* For waits that can last: spin for the first millisecond (most finish by
 * then), then sleep until a socket event or 1 ms, so a waiting kd (priority
 * 2) or server does not keep the CPU from lower-priority tasks. */
static void pause(int sn, uint32_t start)
{
    if (jiffies - start >= 1u && sched_running()) w5500_wait(sn, 1);
}

int w5500_push(int sn)
{
    /* Only one SEND may be in flight: a SEND issued before the previous SEND_OK
     * is ignored, which loses the tail of a stream. */
    if (wait_send_ok(sn, 5000) || command(sn, CR_SEND)) return -1;
    send_pending[sn] = 1;
    queued[sn] = 0;
    return 0;
}

int w5500_send(int sn, const void *buf, uint16_t n, uint32_t timeout_ms)
{
    uint32_t start = jiffies;
    while (w5500_tx_room(sn) < n) {
        if (!w5500_can_send(sn) || jiffies - start > timeout_ms) return -1;
        pause(sn, start);
    }
    if (w5500_write(sn, buf, n)) return -1;
    return w5500_push(sn);
}

int w5500_flush(int sn, uint32_t timeout_ms)
{
    if (wait_send_ok(sn, timeout_ms)) return -1;
    uint32_t start = jiffies;
    while (w5500_tx_free(sn) != tx_size[sn]) {
        if (!w5500_can_send(sn) || jiffies - start > timeout_ms) return -1;
        pause(sn, start);
    }
    return 0;
}

/* Once both FINs have been exchanged (CLOSING, TIME_WAIT, LAST_ACK) there is
 * nothing left to wait for: close at once. Measured: when the peer's FIN and
 * its ACK of ours arrive in one segment, the chip stays in CLOSING and answers
 * every RST of the peer with another ACK, about 28 000 packets a second until
 * the timeout here closed it (1 s at every connection's end). */
void w5500_disconnect(int sn, uint32_t timeout_ms)
{
    command(sn, CR_DISCON);
    uint32_t start = jiffies;
    for (;;) {
        uint8_t sr = w5500_status(sn);
        if (sr == W5500_SOCK_CLOSED || sr == W5500_SOCK_CLOSING || sr == W5500_SOCK_TIME_WAIT ||
            sr == W5500_SOCK_LAST_ACK || jiffies - start >= timeout_ms) break;
        pause(sn, start);
    }
    w5500_close(sn);
}
