/* SPDX-License-Identifier: GPL-3.0-or-later */
/* telnet server: three listening sockets on port 23, one nsh task per connection.
 * Polled by the kernel daemon. */
#include "net.h"
#include "telnet.h"
#include "telnetd.h"
#include "../kernel/klog.h"
#include "../kernel/kmem.h"
#include "../kernel/task.h"
#include "../kernel/tty.h"
#include "../drivers/w5500.h"
#include "../lib/string.h"
#include "../user/nsh.h"

#define PORT 23
#define SESSION_STACK 1280u
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

static int session_poll(void *ctx)
{
    struct session *s = ctx;
    for (;;) {
        while (s->rpos < s->rlen) {
            int c = telnet_rx(&s->tn, s->rx[s->rpos++]);
            if (c >= 0) return c;
        }
        if (!w5500_rx_available(s->sn)) return -1;
        int n = net_recv(s->sn, s->rx, sizeof s->rx, 1);
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

static const struct tty_ops session_ops = { session_getc, session_write, session_poll };

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

/* Two steps: reap the shell and close the connection (its children were hung
 * up and readers now see end of file), then free the session once no task
 * uses its terminal any more. */
static void finish(int i)
{
    struct session *s = sessions[i];
    if (s->task) {
        task_join(s->task, 0);       /* already a zombie: returns at once */
        s->task = 0;
        net_close(s->sn);
    }
    if (task_tty_users(&s->tty)) return;
    kfree(s);
    sessions[i] = 0;
    kprintf("telnetd: session %d closed\n", i);
}

void telnetd_poll(void)
{
    for (int i = 0; i < NET_TELNET_SOCKS; ++i) {
        struct session *s = sessions[i];
        if (s) { if (s->done && (!s->task || s->task->state == TASK_ZOMBIE)) finish(i); }
        else if (listener_poll(&listeners[i])) start(i);
    }
}

void telnetd_init(void)
{
    for (int i = 0; i < NET_TELNET_SOCKS; ++i)
        listeners[i] = (struct listener){ .sn = (uint8_t)(NET_SOCK_TELNET + i), .port = PORT, .name = "telnetd",
                                          .last_sr = 0xff, .announced = (uint8_t)(i != 0) };
}
