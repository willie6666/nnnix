/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "telnet.h"

enum { DATA, IAC, OPTION, SUB, SUB_IAC };

const uint8_t telnet_greeting[9] = {
    TELNET_IAC, TELNET_WILL, TELNET_OPT_ECHO,
    TELNET_IAC, TELNET_WILL, TELNET_OPT_SGA,
    TELNET_IAC, TELNET_DO, TELNET_OPT_SGA,
};

int telnet_rx(struct telnet *t, uint8_t c)
{
    switch (t->state) {
    case DATA:
        if (c == TELNET_IAC) { t->state = IAC; return -1; }
        return c;
    case IAC:
        if (c == TELNET_IAC) { t->state = DATA; return 0xff; }
        if (c >= TELNET_WILL && c <= TELNET_DONT) { t->state = OPTION; return -1; }
        t->state = c == TELNET_SB ? SUB : DATA;     /* NOP, GA, AYT, ... are ignored */
        return -1;
    case OPTION:                                     /* the option byte; requests are not answered */
        t->state = DATA;
        return -1;
    case SUB:
        if (c == TELNET_IAC) t->state = SUB_IAC;
        return -1;
    default:                                         /* SUB_IAC */
        t->state = c == TELNET_SE ? DATA : SUB;
        return -1;
    }
}
