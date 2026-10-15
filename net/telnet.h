/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NNNIX_TELNET_H
#define NNNIX_TELNET_H
#include <stdint.h>

/* Minimal telnet (RFC 854) receive filter: strips option negotiation and
 * subnegotiation, turns IAC IAC into a 0xff data byte. Host tested. */
enum { TELNET_IAC = 255, TELNET_DONT = 254, TELNET_DO = 253, TELNET_WONT = 252,
       TELNET_WILL = 251, TELNET_SB = 250, TELNET_SE = 240,
       TELNET_OPT_ECHO = 1, TELNET_OPT_SGA = 3 };

struct telnet { uint8_t state; };

/* Returns the data byte, or -1 when the byte belonged to a telnet command. */
int telnet_rx(struct telnet *t, uint8_t c);

/* What the server sends on connect: it echoes and suppresses go-ahead, which puts
 * common clients into character-at-a-time mode with remote echo. */
extern const uint8_t telnet_greeting[9];

#endif
