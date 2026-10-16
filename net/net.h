/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NNNIX_NET_H
#define NNNIX_NET_H
#include <stdint.h>

#include "../drivers/w5500.h"
#include "../lib/sys.h"

/* Socket layer over the W5500. One hardware socket is one TCP connection.
 * Socket plan (buffer KiB per direction): 0 diag (4), 1-3 telnet (2 each),
 * 4 HTTP (2), 5-6 programs (2, 1), 7 DHCP (1). */
#define NET_SOCK_DIAG   0
#define NET_SOCK_TELNET 1             /* 1, 2, 3 */
#define NET_TELNET_SOCKS 3
#define NET_SOCK_HTTP   4
#define NET_SOCK_USER   5             /* 5, 6 */
#define NET_USER_SOCKS  2
#define NET_SOCK_DHCP   7
extern const uint8_t net_buffers_kib[8];

/* Interface configuration (netcfg.c). */
extern struct w5500_netcfg net_cfg;
extern uint8_t net_dns[4];
extern volatile uint8_t net_services;    /* NN_SVC_* */
void net_apply(void);                     /* write the addresses to the chip, if present */
int  net_config(struct nn_netcfg *c, int op);
unsigned net_prefix(const uint8_t mask[4]);   /* 0-32, or 33 for a non-contiguous mask */

/* DHCP client (dhcp.c): any task starts it, the kernel daemon runs it. */
void net_set_lease(const uint8_t ip[4], const uint8_t mask[4], const uint8_t gw[4], const uint8_t dns[4]);
void dhcp_start(void);
void dhcp_stop(void);
void dhcp_poll(void);
void dhcp_status(struct nn_netcfg *c);

/* Sockets as files for programs (socket.c). */
int sock_create(int proto);
int sock_connect(int fd, const struct nn_sockaddr *a);
int sock_listen(int fd, uint16_t port);
int sock_info(int fd, struct nn_sockinfo *out);

/* Blocking stream calls for an established socket. */
int  net_recv(int sn, void *buf, uint16_t n, uint32_t timeout_ms); /* bytes; 0 when closed; -ETIMEDOUT */
int  net_send(int sn, const void *buf, uint32_t n);                /* n, or -1 when the connection failed */
void net_close(int sn);
void net_peer(int sn, uint8_t ip[4], uint16_t *port);

/* A listening server socket that re-arms itself: re-listens after CLOSED or
 * CLOSE_WAIT and drops half-open connections stuck in SYN_RECV for over 2 s. */
struct listener {
    uint8_t sn, last_sr, announced;
    uint16_t port;
    uint32_t since;
    const char *name;
};
/* 1 when a connection is established. With accept 0 the service is stopped:
 * the socket is closed instead of listening. */
int  listener_poll(struct listener *l, int accept);

#endif
