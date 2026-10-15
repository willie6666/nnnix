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
uint8_t  w5500_phy(void);                           /* PHYCFGR: bit0 link, bit1 100M, bit2 full */
void     w5500_configure(const struct w5500_netcfg *cfg, const uint8_t buffer_kib[8]);
int      w5500_tcp_listen(int sn, uint16_t port);   /* 0 on LISTEN */
uint8_t  w5500_status(int sn);
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
