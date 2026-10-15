/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NNNIX_NET_H
#define NNNIX_NET_H
#include <stdint.h>

/* Socket layer over the W5500. One hardware socket is one TCP connection.
 * Socket plan: 0 diag (8 KiB), 1-3 telnet (2 KiB each), 4 spare (2 KiB). */
#define NET_SOCK_DIAG   0
#define NET_SOCK_TELNET 1             /* 1, 2, 3 */
#define NET_TELNET_SOCKS 3

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
int  listener_poll(struct listener *l);   /* 1 when a connection is established */

#endif
