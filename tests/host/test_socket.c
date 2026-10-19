/* SPDX-License-Identifier: GPL-3.0-or-later */
/* net/socket.c's UDP reads against a W5500 model whose transfers can fail
 * (review R50): a failed transfer of a datagram's header, of its data or of
 * the skip past a stranger's datagram makes the read fail and starts the
 * socket again, never handing out header bytes as data; reads go on working
 * afterwards. */
#include <stdlib.h>
#include <string.h>
#include "../../net/net.h"
#include "../../fs/vfs.h"
#include "../../kernel/task.h"
#include "../../kernel/sync.h"
#include "../../drivers/w5500.h"
#include "check.h"

/* ---- the model: one UDP socket's receive buffer ---- */
static uint8_t rx[512];
static uint32_t rd, wr, reopens;
static int fail_recv = -1, fail_skip;   /* fail_recv: which recv call fails (0: the next), -1 none */
static uint16_t port = 5353;

static void datagram(const uint8_t ip[4], uint16_t from, const char *data)
{
    uint16_t n = (uint16_t)strlen(data);
    uint8_t h[8] = { ip[0], ip[1], ip[2], ip[3], (uint8_t)(from >> 8), (uint8_t)from, (uint8_t)(n >> 8), (uint8_t)n };
    memcpy(rx + wr, h, 8);
    memcpy(rx + wr + 8, data, n);
    wr += 8u + n;
}

uint16_t w5500_rx_available(int sn) { (void)sn; return (uint16_t)(wr - rd); }
int w5500_recv(int sn, void *buf, uint16_t n)
{
    (void)sn;
    if (n > wr - rd) n = (uint16_t)(wr - rd);
    if (!n) return 0;
    memcpy(buf, rx + rd, n);                  /* read, but the pointer write may fail */
    if (fail_recv == 0) { fail_recv = -1; return -1; }
    if (fail_recv > 0) fail_recv--;
    rd += n;
    return n;
}
int w5500_skip(int sn, uint16_t n)
{
    (void)sn;
    if (fail_skip) { fail_skip = 0; return -1; }
    if (n > wr - rd) n = (uint16_t)(wr - rd);
    rd += n;
    return 0;
}
int w5500_open(int sn, uint8_t mode, uint16_t p) { (void)sn; (void)mode; port = p; rd = wr = 0; reopens++; return 0; }
uint8_t w5500_status(int sn) { (void)sn; return W5500_SOCK_UDP; }
uint16_t w5500_port(int sn) { (void)sn; return port; }
uint8_t w5500_mode(int sn) { (void)sn; return W5500_MODE_UDP; }
void w5500_peer(int sn, uint8_t ip[4], uint16_t *p) { (void)sn; memset(ip, 0, 4); *p = 0; }
uint16_t w5500_tx_room(int sn) { (void)sn; return 0; }
int w5500_wait(int sn, uint32_t ms) { (void)sn; (void)ms; return 0; }
int w5500_can_send(int sn) { (void)sn; return 0; }
void w5500_close(int sn) { (void)sn; }
int w5500_flush(int sn, uint32_t ms) { (void)sn; (void)ms; return 0; }
void w5500_set_peer(int sn, const uint8_t ip[4], uint16_t p) { (void)sn; (void)ip; (void)p; }
int w5500_push(int sn) { (void)sn; return 0; }
int w5500_write(int sn, const void *b, uint16_t n) { (void)sn; (void)b; (void)n; return 0; }
int w5500_tcp_connect(int sn, const uint8_t ip[4], uint16_t p, uint16_t local) { (void)sn; (void)ip; (void)p; (void)local; return 0; }
int w5500_tcp_listen(int sn, uint16_t p) { (void)sn; (void)p; return 0; }

/* ---- the rest of the kernel ---- */
volatile uint32_t jiffies;
const uint8_t net_buffers_kib[8] = { 4, 2, 2, 2, 2, 2, 1, 1 };
static struct task me;
struct task *task_self(void) { return &me; }
void *kmalloc(size_t n) { return malloc(n); }
void kfree(void *p) { free(p); }
void mutex_lock(struct mutex *m) { (void)m; }
int mutex_lock_killable(struct mutex *m) { (void)m; return 0; }
void mutex_unlock(struct mutex *m) { (void)m; }
int net_send(int sn, const void *b, uint32_t n) { (void)sn; (void)b; return (int)n; }
void net_close(int sn) { (void)sn; }
static struct file *files[8];
int vfs_install(struct file *f) { files[3] = f; return 3; }
struct file *vfs_get(int fd) { return fd >= 0 && fd < 8 ? files[fd] : 0; }

static int rd_sock(char *buf, uint32_t n)
{
    struct file *f = files[3];
    int r = f->mnt->ops->read(f, buf, n);
    if (r > 0) buf[r] = 0;
    return r;
}

int main(void)
{
    static const uint8_t peer[4] = { 198, 18, 0, 1 }, stranger[4] = { 10, 0, 0, 9 };
    char buf[64];
    int fd = sock_create(NN_SOCK_UDP);
    CHECK_EQ_INT(fd, 3);
    CHECK_EQ_INT(sock_listen(fd, 5353), 0);

    datagram(peer, 53, "ABC");
    CHECK_EQ_INT(rd_sock(buf, sizeof buf), 3);
    CHECK(!strcmp(buf, "ABC"));

    /* The header's transfer fails: no "data" made of the header. */
    uint32_t before = reopens;
    datagram(peer, 53, "ABC");
    fail_recv = 0;
    CHECK_EQ_INT(rd_sock(buf, sizeof buf), SYS_EIO);
    CHECK_EQ_INT(reopens, before + 1);
    CHECK_EQ_INT(port, 5353);                                        /* again on its own port */
    datagram(peer, 53, "XYZ");
    CHECK_EQ_INT(rd_sock(buf, sizeof buf), 3);
    CHECK(!strcmp(buf, "XYZ"));

    /* The data's transfer fails. */
    datagram(peer, 53, "DEF");
    fail_recv = 1;
    CHECK_EQ_INT(rd_sock(buf, sizeof buf), SYS_EIO);
    datagram(peer, 53, "GHI");
    CHECK_EQ_INT(rd_sock(buf, sizeof buf), 3);
    CHECK(!strcmp(buf, "GHI"));

    /* A short read drops the rest of the datagram; that skip fails. */
    datagram(peer, 53, "LONGER");
    fail_skip = 1;
    CHECK_EQ_INT(rd_sock(buf, 2), SYS_EIO);
    datagram(peer, 53, "OK");
    CHECK_EQ_INT(rd_sock(buf, sizeof buf), 2);
    CHECK(!strcmp(buf, "OK"));

    /* Connected: a stranger's datagram is skipped; that skip fails. */
    struct nn_sockaddr a = { { 198, 18, 0, 1 }, 53 };
    CHECK_EQ_INT(sock_connect(fd, &a), 0);
    datagram(stranger, 53, "BAD");
    datagram(peer, 53, "GOOD");
    fail_skip = 1;
    CHECK_EQ_INT(rd_sock(buf, sizeof buf), SYS_EIO);
    datagram(stranger, 53, "BAD");
    datagram(peer, 53, "GOOD");
    CHECK_EQ_INT(rd_sock(buf, sizeof buf), 4);
    CHECK(!strcmp(buf, "GOOD"));
    return check_summary("test_socket");
}
