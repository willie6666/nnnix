/* SPDX-License-Identifier: GPL-3.0-or-later */
/* bin/wget.c against a scripted server (review R60): a body received whole
 * into a file whose close fails is not reported as saved; with the close
 * working it is. */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "../../lib/sys.h"
#include "check.h"

int cmd_wget(int argc, char **argv);

static const char response[] = "HTTP/1.0 200 OK\r\nContent-Length: 3\r\n\r\nABC";
static uint32_t sent, now, written, closes;
static int close_result;
static uint8_t heap[4096];

intptr_t host_syscall(int n, intptr_t a, intptr_t b, intptr_t c)
{
    switch (n) {
    case SYS_ALLOC: return (intptr_t)heap;
    case SYS_FREE: case SYS_CONNECT: return 0;
    case SYS_SOCKET: return 3;
    case SYS_UPTIME: return (intptr_t)now;
    case SYS_SLEEP: now += (uint32_t)a; return 0;
    case SYS_OPEN: return 4;
    case SYS_CLOSE: if (a == 4) { closes++; return close_result; } return 0;
    case SYS_SOCKINFO: {
        struct nn_sockinfo *i = (struct nn_sockinfo *)b;
        memset(i, 0, sizeof *i);
        i->state = sent < sizeof response - 1 ? NN_SS_ESTABLISHED : NN_SS_CLOSE_WAIT;
        i->rx = (uint16_t)(sizeof response - 1 - sent);
        return 0;
    }
    case SYS_READ: {
        uint32_t k = (uint32_t)(sizeof response - 1 - sent);
        if (k > (uint32_t)c) k = (uint32_t)c;
        memcpy((void *)b, response + sent, k);
        sent += k;
        return (intptr_t)k;
    }
    case SYS_WRITE:
        if (a == 4) written += (uint32_t)c;
        return c;
    default:
        return SYS_ENOSYS;
    }
}

int nn_interrupted(void) { return 0; }
const char *errstr(int e) { (void)e; return "error"; }
int nn_write_all(int fd, const void *buf, size_t n) { return (int)host_syscall(SYS_WRITE, fd, (intptr_t)buf, (intptr_t)n); }

static int get(int result)
{
    char *argv[] = { "wget", "-q", "-O", "/home/x", "http://198.18.0.1:8000/x", 0 };
    sent = written = closes = 0;
    close_result = result;
    return cmd_wget(5, argv);
}

int main(void)
{
    CHECK_EQ_INT(get(0), 0);
    CHECK_EQ_INT(written, 3);
    CHECK_EQ_INT(get(SYS_EIO), 1);                              /* all received, but not saved */
    CHECK_EQ_INT(written, 3);
    CHECK_EQ_INT(closes, 1);
    return check_summary("test_wget");
}
