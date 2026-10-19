/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The board initiates TCP; a split frame and input share that connection. */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "../../lib/sys.h"
#include "check.h"

int cmd_remote(int argc, char **argv);

static uint8_t heap[2048], stream[16 + 3 + 1024];
static uint32_t now, at, length;
static int sockets, connects, closed, allocated, freed, lcd_open, input_open, frames, events;
static int mode, polls, interrupted, event_sent;
static struct nn_sockaddr peer;
enum { NORMAL, REFUSED, TIMEOUT, CANCEL, INFO_ERROR, FAST_CLOSE, CUT };

intptr_t host_syscall(int n, intptr_t a, intptr_t b, intptr_t c)
{
    switch (n) {
    case SYS_ALLOC: allocated++; return (intptr_t)heap;
    case SYS_FREE: freed++; return 0;
    case SYS_SOCKET: sockets++; return 3;
    case SYS_CONNECT: connects++; peer = *(const struct nn_sockaddr *)b; return 0;
    case SYS_LISTEN: CHECK(0); return SYS_ENOSYS;
    case SYS_UPTIME: return now;
    case SYS_SLEEP: now += (uint32_t)a; return 0;
    case SYS_OPEN:
        if (!strcmp((const char *)a, "/dev/lcd")) { lcd_open++; return 4; }
        input_open++; return 5;
    case SYS_CLOSE: closed |= 1 << a; return 0;
    case SYS_SOCKINFO: {
        struct nn_sockinfo *i = (struct nn_sockinfo *)b;
        memset(i, 0, sizeof *i);
        if (mode == INFO_ERROR) return SYS_EIO;
        if (mode == REFUSED) return 0;
        if (mode == TIMEOUT || mode == CANCEL || polls++ < 2) { i->state = NN_SS_SYNSENT; return 0; }
        i->state = at == length || mode == FAST_CLOSE ? NN_SS_CLOSE_WAIT : NN_SS_ESTABLISHED;
        i->rx = (uint16_t)(length - at);
        return 0;
    }
    case SYS_READ:
        if (a == 5) {
            if (event_sent) return SYS_EAGAIN;
            struct nn_input_event ev = { .type = 1, .code = 4, .value = 1 };
            memcpy((void *)b, &ev, sizeof ev);
            event_sent = 1;
            return sizeof ev;
        } else {
            uint32_t k = length - at;
            if (k > 7) k = 7;                 /* headers and frames cross read boundaries */
            if (k > (uint32_t)c) k = (uint32_t)c;
            memcpy((void *)b, stream + at, k);
            at += k;
            return k;
        }
    case SYS_WRITE:
        if (a == 4) {
            CHECK_EQ_INT(c, 1024);
            CHECK_EQ_INT(((uint8_t *)b)[0], 0x5a);
            CHECK_EQ_INT(((uint8_t *)b)[1023], 0x5a);
            frames++;
        } else if (a == 3) {
            CHECK_EQ_INT(c, sizeof(struct nn_input_event));
            CHECK_EQ_INT(((struct nn_input_event *)b)->code, 4);
            events++;
        }
        return c;
    default: return SYS_ENOSYS;
    }
}

int nn_interrupted(void) { return mode == CANCEL && interrupted++ > 0; }
const char *errstr(int e) { (void)e; return "error"; }

static int invoke(int test, int argc, char **argv)
{
    mode = test;
    sockets = connects = closed = allocated = freed = lcd_open = input_open = frames = events = 0;
    polls = interrupted = event_sent = 0;
    now = at = 0;
    memset(stream, 0, sizeof stream);
    memcpy(stream, "NNV1\x80\0\x40\0", 8);
    stream[18] = 4;                            /* RAW, length 1024 little endian */
    memset(stream + 19, 0x5a, 1024);
    length = mode == CUT ? 20 : sizeof stream;
    return cmd_remote(argc, argv);
}

int main(void)
{
    char *ok[] = { "remote", "-v", "198.18.0.1", "4321" };
    for (int m = NORMAL; m <= CUT; ++m) {
        CHECK_EQ_INT(invoke(m, 4, ok), m == CANCEL ? 130 : m == NORMAL || m == FAST_CLOSE ? 0 : 1);
        CHECK_EQ_INT(sockets, 1);
        CHECK_EQ_INT(connects, 1);
        CHECK(!memcmp(peer.ip, "\xc6\x12\0\1", 4));
        CHECK_EQ_INT(peer.port, 4321);
        CHECK_EQ_INT(allocated, freed);
        CHECK(closed & (1 << 3));
        if (m == NORMAL || m == FAST_CLOSE || m == CUT) {
            CHECK_EQ_INT(lcd_open, 1);
            CHECK_EQ_INT(input_open, 1);
            CHECK_EQ_INT(frames, m == CUT ? 0 : 1);
            CHECK_EQ_INT(events, 1);
            CHECK_EQ_INT(closed, (1 << 3) | (1 << 4) | (1 << 5));
        } else {
            CHECK_EQ_INT(lcd_open, 0);
            CHECK_EQ_INT(input_open, 0);
        }
        if (m == TIMEOUT) CHECK(now >= 10000 && now <= 10020);
    }
    const char *bad_ip[] = { "", "1.2.3", "1.2.3.4.5", "256.1.2.3", "123456789.1.2.3", "1..2.3", "1.2.3.-1", "1.2.3.4x" };
    const char *bad_port[] = { "", "0", "65536", "999999999999", "-1", "123x" };
    for (unsigned i = 0; i < sizeof bad_ip / sizeof *bad_ip; ++i) {
        char *args[] = { "remote", (char *)bad_ip[i], "1234" };
        CHECK_EQ_INT(invoke(NORMAL, 3, args), 2);
        CHECK_EQ_INT(allocated + sockets, 0);
    }
    for (unsigned i = 0; i < sizeof bad_port / sizeof *bad_port; ++i) {
        char *args[] = { "remote", "1.2.3.4", (char *)bad_port[i] };
        CHECK_EQ_INT(invoke(NORMAL, 3, args), 2);
        CHECK_EQ_INT(allocated + sockets, 0);
    }
    char *extra[] = { "remote", "1.2.3.4", "1234", "1235" };
    CHECK_EQ_INT(invoke(NORMAL, 4, extra), 2);
    CHECK_EQ_INT(invoke(NORMAL, 1, extra), 2);
    CHECK_EQ_INT(invoke(NORMAL, 2, extra), 2);
    return check_summary("test_remote");
}
