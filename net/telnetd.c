/* SPDX-License-Identifier: GPL-3.0-or-later */
/* telnet server: three listening sockets on port 23, one nsh task per connection. */
#include "net.h"
#include "telnet.h"
#include "telnetd.h"
#include "../kernel/klog.h"
#include "../kernel/kmem.h"
#include "../kernel/modules.h"
#include "../kernel/panic.h"
#include "../kernel/task.h"
#include "../kernel/tty.h"
#include "../lib/string.h"
#include "../user/nsh.h"

#define PORT 23
#define SESSION_STACK 1152u
#define SESSION_PRIO 5

struct session {
    struct tty tty;
    struct telnet tn;
    struct task *task;
    uint8_t sn, rpos, rlen;
    volatile uint8_t done;
    uint8_t rx[32];
};

static struct session *sessions[NET_TELNET_SOCKS];
static struct listener listeners[NET_TELNET_SOCKS];

static int session_getc(void *ctx)
{
    struct session *s = ctx;
    for (;;) {
        while (s->rpos < s->rlen) {
            int c = telnet_rx(&s->tn, s->rx[s->rpos++]);
            if (c >= 0) return c;
        }
        int n = net_recv(s->sn, s->rx, sizeof s->rx, 0);
        if (n <= 0) return -1;
        s->rlen = (uint8_t)n;
        s->rpos = 0;
    }
}

static void session_write(void *ctx, const char *p, size_t n)
{
    struct session *s = ctx;
    net_send(s->sn, p, (uint32_t)n);  /* text only: a 0xff byte would need IAC doubling */
}

static const struct tty_ops session_ops = { session_getc, session_write };

static void session_main(void *arg)
{
    struct session *s = arg;
    task_self()->tty = &s->tty;
    nsh_main();
    s->done = 1;
}

static void start(int i)
{
    uint8_t sn = (uint8_t)(NET_SOCK_TELNET + i), ip[4];
    uint16_t port;
    net_peer(sn, ip, &port);
    struct session *s = kmalloc(sizeof *s);
    if (s) {
        memset(s, 0, sizeof *s);
        s->sn = sn;
        tty_init(&s->tty, &session_ops, s);
        net_send(sn, telnet_greeting, sizeof telnet_greeting);
        s->task = task_spawn("nsh", session_main, s, SESSION_STACK, SESSION_PRIO, 0);
    }
    if (!s || !s->task) {
        static const char msg[] = "nnnix: out of memory\r\n";
        net_send(sn, msg, sizeof msg - 1);
        net_close(sn);
        kfree(s);
        kprintf("telnetd: refused %u.%u.%u.%u:%u, out of memory\n", ip[0], ip[1], ip[2], ip[3], port);
        return;
    }
    sessions[i] = s;
    kprintf("telnetd: session %d (pid %u) from %u.%u.%u.%u:%u\n", i, s->task->pid, ip[0], ip[1], ip[2], ip[3], port);
}

static void finish(int i)
{
    struct session *s = sessions[i];
    task_join(s->task, 0);
    net_close(s->sn);
    kfree(s);
    sessions[i] = 0;
    kprintf("telnetd: session %d closed\n", i);
}

static void telnetd_main(void *arg)
{
    (void)arg;
    for (int i = 0; i < NET_TELNET_SOCKS; ++i)
        listeners[i] = (struct listener){ .sn = (uint8_t)(NET_SOCK_TELNET + i), .port = PORT, .name = "telnetd",
                                          .last_sr = 0xff, .announced = (uint8_t)(i != 0) };
    for (;;) {
        if (modules.w5500) {
            for (int i = 0; i < NET_TELNET_SOCKS; ++i) {
                if (sessions[i]) { if (sessions[i]->done) finish(i); }
                else if (listener_poll(&listeners[i])) start(i);
            }
        }
        task_sleep(50);
    }
}

void telnetd_init(void)
{
    if (!task_spawn("telnetd", telnetd_main, 0, 768, 3, 1)) panic("cannot start telnetd");
}
