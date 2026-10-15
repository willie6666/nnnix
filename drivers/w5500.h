/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NNNIX_W5500_H
#define NNNIX_W5500_H
#include <stdint.h>

/* W5500 hardware TCP/IP over SPI1. Socket data lives in the chip's 32 KiB buffer. */
#define W5500_SOCK_CLOSED      0x00
#define W5500_SOCK_INIT        0x13
#define W5500_SOCK_LISTEN      0x14
#define W5500_SOCK_SYNRECV     0x16
#define W5500_SOCK_ESTABLISHED 0x17
#define W5500_SOCK_CLOSE_WAIT  0x1c

struct w5500_netcfg { uint8_t mac[6], ip[4], mask[4], gateway[4]; };

int      w5500_present(void);                       /* VERSIONR == 0x04 */
/* Interrupts: sockets with a buffer get SEND_OK/TIMEOUT/RECV/DISCON/CON enabled.
 * w5500_isr() runs from the PB2 GPIO interrupt; the netd task waits with
 * w5500_irq_wait() and calls w5500_collect(), which acknowledges the chip and
 * wakes tasks blocked in w5500_wait() on the affected sockets. */
void     w5500_isr(void);
int      w5500_irq_wait(uint32_t timeout_ms);
uint8_t  w5500_collect(void);                       /* mask of sockets that had events */
int      w5500_wait(int sn, uint32_t timeout_ms);   /* 0 on an event, -ETIMEDOUT otherwise */
uint8_t  w5500_phy(void);                           /* PHYCFGR: bit0 link, bit1 100M, bit2 full */
void     w5500_configure(const struct w5500_netcfg *cfg, const uint8_t buffer_kib[8]);
int      w5500_tcp_listen(int sn, uint16_t port);   /* 0 on LISTEN */
uint8_t  w5500_status(int sn);
void     w5500_peer(int sn, uint8_t ip[4], uint16_t *port);
uint16_t w5500_rx_available(int sn);
uint16_t w5500_recv(int sn, void *buf, uint16_t n); /* copies up to n available bytes */
uint16_t w5500_tx_free(int sn);                    /* as of the last SEND */
uint16_t w5500_tx_room(int sn);                    /* free minus data queued since */
int      w5500_write(int sn, const void *buf, uint16_t n); /* queue; n <= tx_room, no SEND */
int      w5500_push(int sn);                        /* SEND queued data, pipelined */
int      w5500_send(int sn, const void *buf, uint16_t n, uint32_t timeout_ms);
int      w5500_flush(int sn, uint32_t timeout_ms);  /* until the peer ACKed everything */
void     w5500_disconnect(int sn, uint32_t timeout_ms);

#endif
