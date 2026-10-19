/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Sockets for programs: W5500 sockets 5 and 6 as files.
 *
 * connect and listen only start the work; programs poll sockinfo for the state,
 * so they can watch for Ctrl-C meanwhile. Reads wait for data and can be
 * interrupted by a kill. A UDP read returns one datagram. On a listening UDP
 * socket its sender becomes the destination of following writes; a connected
 * one keeps its destination and drops datagrams from anyone else (the read
 * then returns SYS_EAGAIN if nothing else is waiting). */
#include "net.h"
#include "../fs/vfs.h"
#include "../kernel/kmem.h"
#include "../kernel/task.h"
#include "../kernel/sync.h"
#include "../arch/nuc140/arch.h"
#include "../lib/string.h"

struct sock {
    struct mutex wlock, rlock;       /* a parent and its children may share the socket */
    uint8_t sn, proto, has_peer, connected;
    uint16_t left;                   /* UDP: unread bytes of the current datagram */
    uint8_t peer[4];
    uint16_t peer_port;
};

static uint8_t in_use;               /* bit per program socket */
static uint16_t next_port;

static int killed(void) { return task_self()->killed; }

static uint16_t ephemeral(void)
{
    if (!next_port) next_port = (uint16_t)(49152u + (jiffies & 0x1fffu));
    uint16_t p = next_port++;
    if (next_port < 49152u) next_port = 49152u;
    return p;
}

static int tcp_read(struct sock *s, void *buf, uint32_t n)
{
    if (n > 0xffffu) n = 0xffffu;
    for (;;) {
        int got = w5500_recv(s->sn, buf, (uint16_t)n);
        if (got < 0) return SYS_EIO;
        if (got) return got;
        uint8_t sr = w5500_status(s->sn);
        if (sr != W5500_SOCK_ESTABLISHED && sr != W5500_SOCK_SYNSENT && sr != W5500_SOCK_LISTEN &&
            sr != W5500_SOCK_SYNRECV) return 0;
        if (killed()) return SYS_EINTR;
        w5500_wait(s->sn, 100);
    }
}

/* After a failed transfer the chip's read pointer is unknown, so the next
 * bytes could be anything (a header taken for data, review R50): the socket
 * starts again on its port, dropping what it had received, and the read fails. */
static int udp_resync(struct sock *s)
{
    s->left = 0;
    (void)w5500_open(s->sn, W5500_MODE_UDP, w5500_port(s->sn));
    return SYS_EIO;
}

static int udp_read(struct sock *s, void *buf, uint32_t n)
{
    while (!s->left) {
        if (w5500_rx_available(s->sn) >= 8) {
            uint8_t h[8];
            if (w5500_recv(s->sn, h, 8) != 8) return udp_resync(s);   /* sender address, port, length */
            uint16_t port = (uint16_t)(h[4] << 8 | h[5]), len = (uint16_t)(h[6] << 8 | h[7]);
            if (s->connected && (memcmp(s->peer, h, 4) || s->peer_port != port)) {
                if (w5500_skip(s->sn, len)) return udp_resync(s);   /* not from the peer */
                if (w5500_rx_available(s->sn) < 8) return SYS_EAGAIN;
                continue;
            }
            if (!s->connected) {
                memcpy(s->peer, h, 4);
                s->peer_port = port;
                s->has_peer = 1;
            }
            s->left = len;
            if (!s->left) return 0;
            break;
        }
        if (w5500_status(s->sn) != W5500_SOCK_UDP) return SYS_EBADF;
        if (killed()) return SYS_EINTR;
        w5500_wait(s->sn, 100);
    }
    uint16_t k = n < s->left ? (uint16_t)n : s->left;
    int got = w5500_recv(s->sn, buf, k);
    if (got != k) return udp_resync(s);            /* the datagram is all there: less is a failure */
    s->left = (uint16_t)(s->left - got);
    if (s->left && w5500_skip(s->sn, s->left)) return udp_resync(s);
    s->left = 0;
    return got;
}

/* One reader at a time: the W5500 read pointer and a UDP datagram's header
 * and remaining length are updated over several steps. */
static int s_read(struct file *f, void *buf, uint32_t n)
{
    struct sock *s = f->priv;
    if (mutex_lock_killable(&s->rlock)) return SYS_EINTR;
    int r = s->proto == NN_SOCK_TCP ? tcp_read(s, buf, n) : udp_read(s, buf, n);
    mutex_unlock(&s->rlock);
    return r;
}

static int udp_write(struct sock *s, const void *buf, uint32_t n);

static int s_write(struct file *f, const void *buf, uint32_t n)
{
    struct sock *s = f->priv;
    mutex_lock(&s->wlock);
    int r = s->proto == NN_SOCK_TCP ? (net_send(s->sn, buf, n) < 0 ? SYS_EPIPE : (int)n) : udp_write(s, buf, n);
    mutex_unlock(&s->wlock);
    return r;
}

static int udp_write(struct sock *s, const void *buf, uint32_t n)
{
    if (!s->has_peer) return SYS_EINVAL;
    if (w5500_status(s->sn) != W5500_SOCK_UDP) return SYS_EBADF;
    if (n > net_buffers_kib[s->sn] * 1024u) return SYS_EINVAL;
    if (w5500_flush(s->sn, 3000)) return SYS_EIO;     /* the previous datagram has left */
    w5500_set_peer(s->sn, s->peer, s->peer_port);
    if (w5500_write(s->sn, buf, (uint16_t)n) || w5500_push(s->sn)) return SYS_EIO;
    /* Wait for SEND_OK so an unreachable address (no ARP answer) is reported. */
    return w5500_flush(s->sn, 3000) ? SYS_EIO : (int)n;
}

static int s_close(struct file *f)
{
    struct sock *s = f->priv;
    if (s->proto == NN_SOCK_TCP && w5500_can_send(s->sn)) net_close(s->sn);
    else w5500_close(s->sn);
    uint32_t irq = irq_save();
    in_use &= (uint8_t)~(1u << (s->sn - NET_SOCK_USER));
    irq_restore(irq);
    kfree(s);
    return 0;
}

static const struct fs_ops sock_ops = { .close = s_close, .read = s_read, .write = s_write };
static struct mount sock_mount = { "socket:", &sock_ops, 0, 0 };

int sock_create(int proto)
{
    if (proto != NN_SOCK_TCP && proto != NN_SOCK_UDP) return SYS_EINVAL;
    uint32_t irq = irq_save();
    int i = 0;
    while (i < NET_USER_SOCKS && (in_use & (1u << i))) ++i;
    if (i < NET_USER_SOCKS) in_use |= (uint8_t)(1u << i);
    irq_restore(irq);
    if (i == NET_USER_SOCKS) return SYS_EBUSY;
    struct sock *s = kmalloc(sizeof *s);
    struct file *f = kmalloc(sizeof *f);
    int fd = SYS_ENOMEM;
    if (s && f) {
        *s = (struct sock){ .sn = (uint8_t)(NET_SOCK_USER + i), .proto = (uint8_t)proto };
        *f = (struct file){ .mnt = &sock_mount, .priv = s, .flags = O_RDWR, .refs = 1 };
        fd = vfs_install(f);
        if (fd >= 0) return fd;
    }
    kfree(s);
    kfree(f);
    irq = irq_save();
    in_use &= (uint8_t)~(1u << i);
    irq_restore(irq);
    return fd;
}

static struct sock *sock_of(int fd)
{
    struct file *f = vfs_get(fd);
    return f && f->mnt == &sock_mount ? f->priv : 0;
}

int sock_connect(int fd, const struct nn_sockaddr *a)
{
    struct sock *s = sock_of(fd);
    if (!s) return SYS_EBADF;
    if (!a->port) return SYS_EINVAL;
    memcpy(s->peer, a->ip, 4);
    s->peer_port = a->port;
    s->has_peer = s->connected = 1;
    if (s->proto == NN_SOCK_TCP)
        return w5500_tcp_connect(s->sn, a->ip, a->port, ephemeral()) ? SYS_EIO : 0;
    if (w5500_status(s->sn) != W5500_SOCK_UDP && w5500_open(s->sn, W5500_MODE_UDP, ephemeral())) return SYS_EIO;
    return 0;
}

int sock_listen(int fd, uint16_t port)
{
    struct sock *s = sock_of(fd);
    if (!s) return SYS_EBADF;
    if (!port) return SYS_EINVAL;
    if (s->proto == NN_SOCK_TCP) return w5500_tcp_listen(s->sn, port) ? SYS_EIO : 0;
    s->has_peer = s->connected = 0;
    return w5500_open(s->sn, W5500_MODE_UDP, port) ? SYS_EIO : 0;
}

int sock_info(int fd, struct nn_sockinfo *out)
{
    int sn;
    if (fd < 0) {
        sn = -1 - fd;
        if (sn > 7) return SYS_EINVAL;
    } else {
        struct sock *s = sock_of(fd);
        if (!s) return SYS_EBADF;
        sn = s->sn;
    }
    memset(out, 0, sizeof *out);
    out->sn = (uint8_t)sn;
    out->state = w5500_status(sn);
    uint8_t mode = w5500_mode(sn) & 0x0fu;
    if (out->state != W5500_SOCK_CLOSED) out->proto = mode == 1 ? NN_SOCK_TCP : mode == 2 ? NN_SOCK_UDP : 0;
    out->user = sn >= NET_SOCK_USER && sn < NET_SOCK_USER + NET_USER_SOCKS && (in_use & (1u << (sn - NET_SOCK_USER)));
    out->port = w5500_port(sn);
    w5500_peer(sn, out->peer, &out->peer_port);
    out->rx = w5500_rx_available(sn);
    out->tx_free = w5500_tx_room(sn);
    return 0;
}
