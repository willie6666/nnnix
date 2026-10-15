#include "w5500.h"
#include "spi.h"
#include "../arch/nuc140/arch.h"

/* Block select: 0 common, socket n: registers 1+4n, TX buffer 2+4n, RX buffer 3+4n. */
#define SREG(sn) ((uint8_t)(1u + 4u * (unsigned)(sn)))
#define STX(sn)  ((uint8_t)(2u + 4u * (unsigned)(sn)))
#define SRX(sn)  ((uint8_t)(3u + 4u * (unsigned)(sn)))
enum { Sn_MR = 0x00, Sn_CR = 0x01, Sn_IR = 0x02, Sn_SR = 0x03, Sn_PORT = 0x04,
       Sn_RXBUF = 0x1e, Sn_TXBUF = 0x1f, Sn_TX_FSR = 0x20, Sn_TX_WR = 0x24,
       Sn_RX_RSR = 0x26, Sn_RX_RD = 0x28 };
enum { CR_OPEN = 0x01, CR_LISTEN = 0x02, CR_DISCON = 0x08, CR_CLOSE = 0x10,
       CR_SEND = 0x20, CR_RECV = 0x40 };
enum { IR_DISCON = 0x02, IR_TIMEOUT = 0x08, IR_SEND_OK = 0x10 };

static uint16_t tx_size[8];
static uint16_t queued[8];           /* bytes written since the last SEND */
static uint8_t send_pending;         /* bit per socket: a SEND awaits SEND_OK */

static void header(uint16_t address, uint8_t control)
{
    spi_select(SPI_W5500);
    spi_xfer8((uint8_t)(address >> 8));
    spi_xfer8((uint8_t)address);
    spi_xfer8(control);
}
static void wr(uint16_t address, uint8_t block, const void *p, uint32_t n)
{
    header(address, (uint8_t)((block << 3) | 4u));
    spi_write(p, n);
    spi_select(SPI_NONE);
}
static void rd(uint16_t address, uint8_t block, void *p, uint32_t n)
{
    header(address, (uint8_t)(block << 3));
    spi_read(p, n);
    spi_select(SPI_NONE);
}
static uint8_t r8(uint16_t a, uint8_t b) { uint8_t v; rd(a, b, &v, 1); return v; }
static void w8(uint16_t a, uint8_t b, uint8_t v) { wr(a, b, &v, 1); }
static uint16_t r16(uint16_t a, uint8_t b) { uint8_t v[2]; rd(a, b, v, 2); return (uint16_t)((v[0] << 8) | v[1]); }
static void w16(uint16_t a, uint8_t b, uint16_t v) { uint8_t x[2] = { (uint8_t)(v >> 8), (uint8_t)v }; wr(a, b, x, 2); }
static uint16_t r16_stable(uint16_t a, uint8_t b)
{
    /* FSR/RSR may change between the two byte reads. */
    uint16_t x = r16(a, b), y;
    while ((y = r16(a, b)) != x) x = y;
    return x;
}
static void command(int sn, uint8_t cmd)
{
    w8(Sn_CR, SREG(sn), cmd);
    uint32_t start = jiffies;
    while (r8(Sn_CR, SREG(sn)) && jiffies - start < 100u) { }
}

int w5500_present(void) { return r8(0x0039, 0) == 0x04; }
uint8_t w5500_phy(void) { return r8(0x002e, 0); }

void w5500_configure(const struct w5500_netcfg *cfg, const uint8_t buffer_kib[8])
{
    uint8_t common[18];
    for (int i = 0; i < 4; ++i) common[i] = cfg->gateway[i];
    for (int i = 0; i < 4; ++i) common[4 + i] = cfg->mask[i];
    for (int i = 0; i < 6; ++i) common[8 + i] = cfg->mac[i];
    for (int i = 0; i < 4; ++i) common[14 + i] = cfg->ip[i];
    wr(0x0001, 0, common, sizeof common);          /* GAR SUBR SHAR SIPR are contiguous */
    for (int sn = 0; sn < 8; ++sn) {
        w8(Sn_RXBUF, SREG(sn), buffer_kib[sn]);
        w8(Sn_TXBUF, SREG(sn), buffer_kib[sn]);
        tx_size[sn] = (uint16_t)(buffer_kib[sn] * 1024u);
        queued[sn] = 0;
    }
    send_pending = 0;
}

int w5500_tcp_listen(int sn, uint16_t port)
{
    command(sn, CR_CLOSE);
    w8(Sn_MR, SREG(sn), 0x21);                      /* TCP, no delayed ACK */
    w16(Sn_PORT, SREG(sn), port);
    w8(Sn_IR, SREG(sn), 0x1f);
    send_pending &= (uint8_t)~(1u << sn);
    queued[sn] = 0;
    command(sn, CR_OPEN);
    if (r8(Sn_SR, SREG(sn)) != W5500_SOCK_INIT) return -1;
    command(sn, CR_LISTEN);
    return r8(Sn_SR, SREG(sn)) == W5500_SOCK_LISTEN ? 0 : -1;
}

uint8_t w5500_status(int sn) { return r8(Sn_SR, SREG(sn)); }
uint16_t w5500_rx_available(int sn) { return r16_stable(Sn_RX_RSR, SREG(sn)); }
uint16_t w5500_tx_free(int sn) { return r16_stable(Sn_TX_FSR, SREG(sn)); }

uint16_t w5500_recv(int sn, void *buf, uint16_t n)
{
    uint16_t avail = w5500_rx_available(sn);
    if (n > avail) n = avail;
    if (!n) return 0;
    uint16_t ptr = r16(Sn_RX_RD, SREG(sn));
    rd(ptr, SRX(sn), buf, n);                       /* chip wraps the offset itself */
    w16(Sn_RX_RD, SREG(sn), (uint16_t)(ptr + n));
    command(sn, CR_RECV);
    return n;
}

int w5500_write(int sn, const void *buf, uint16_t n)
{
    /* Sn_TX_WR and Sn_TX_FSR read back their values as of the last SEND (measured),
     * so data queued since then is tracked here; otherwise each write overwrites
     * the previous one at the same offset. */
    if ((uint32_t)n + queued[sn] > w5500_tx_free(sn)) return -1;
    uint16_t ptr = (uint16_t)(r16(Sn_TX_WR, SREG(sn)) + queued[sn]);
    wr(ptr, STX(sn), buf, n);
    w16(Sn_TX_WR, SREG(sn), (uint16_t)(ptr + n));
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
    if (!(send_pending & (1u << sn))) return 0;
    uint32_t start = jiffies;
    for (;;) {
        uint8_t ir = r8(Sn_IR, SREG(sn));
        if (ir & IR_SEND_OK) {
            w8(Sn_IR, SREG(sn), IR_SEND_OK);
            send_pending &= (uint8_t)~(1u << sn);
            return 0;
        }
        if ((ir & (IR_TIMEOUT | IR_DISCON)) || jiffies - start > timeout_ms) return -1;
    }
}

int w5500_push(int sn)
{
    /* Only one SEND may be in flight: a SEND issued before the previous SEND_OK
     * is ignored, which loses the tail of a stream. */
    if (wait_send_ok(sn, 5000)) return -1;
    command(sn, CR_SEND);
    send_pending |= (uint8_t)(1u << sn);
    queued[sn] = 0;
    return 0;
}

int w5500_send(int sn, const void *buf, uint16_t n, uint32_t timeout_ms)
{
    uint32_t start = jiffies;
    while (w5500_tx_room(sn) < n) {
        if (w5500_status(sn) != W5500_SOCK_ESTABLISHED || jiffies - start > timeout_ms) return -1;
    }
    if (w5500_write(sn, buf, n)) return -1;
    return w5500_push(sn);
}

int w5500_flush(int sn, uint32_t timeout_ms)
{
    if (wait_send_ok(sn, timeout_ms)) return -1;
    uint32_t start = jiffies;
    while (w5500_tx_free(sn) != tx_size[sn]) {
        if (w5500_status(sn) != W5500_SOCK_ESTABLISHED || jiffies - start > timeout_ms) return -1;
    }
    return 0;
}

void w5500_disconnect(int sn, uint32_t timeout_ms)
{
    command(sn, CR_DISCON);
    uint32_t start = jiffies;
    while (w5500_status(sn) != W5500_SOCK_CLOSED && jiffies - start < timeout_ms) { }
    command(sn, CR_CLOSE);
    send_pending &= (uint8_t)~(1u << sn);
    queued[sn] = 0;
}
