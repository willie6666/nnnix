/* SPDX-License-Identifier: GPL-3.0-or-later */
/* One transfer at a time, using the W5500's previously spare socket. */
#include "http.h"
#include "net.h"
#include "../fs/vfs.h"
#include "../arch/nuc140/arch.h"
#include "../kernel/kernel.h"
#include "../kernel/kmem.h"
#include "../kernel/task.h"
#include "../lib/fmt.h"
#include "../lib/string.h"

#define HTTP_SOCKET 4
#define IO_TIMEOUT 2000u

struct transfer {
    struct http_request req;
    uint8_t rx[128];
    char line[HTTP_LINE];
    char temp[32];
    int pos, len;
    uint32_t deadline;
};
static struct task *worker;
static struct listener listener = { .sn = HTTP_SOCKET, .port = 8080, .name = "httpd", .last_sr = 0xff };

static int receive(struct transfer *t)
{
    if ((int32_t)(jiffies - t->deadline) >= 0) return -408;
    if (t->pos == t->len) {
        int n = net_recv(HTTP_SOCKET, t->rx, sizeof t->rx, IO_TIMEOUT);
        if (n <= 0) return -408;
        t->pos = 0; t->len = n;
    }
    return 0;
}

static int read_line(struct transfer *t)
{
    int n = 0;
    for (;;) {
        int e = receive(t);
        if (e) return e;
        int c = t->rx[t->pos++];
        if (c == '\n') {
            if (!n || t->line[n - 1] != '\r') return -400;
            t->line[n - 1] = 0;
            return n + 1;
        }
        if (!c || (c < 32 && c != '\r' && c != '\t')) return -400;
        if (n == HTTP_LINE - 1) return -431;
        t->line[n++] = (char)c;
    }
}

static const char *reason(int status)
{
    switch (status) {
    case 200: return "OK"; case 201: return "Created"; case 204: return "No Content";
    case 400: return "Bad Request"; case 403: return "Forbidden"; case 404: return "Not Found";
    case 405: return "Method Not Allowed"; case 408: return "Request Timeout";
    case 409: return "Conflict"; case 411: return "Length Required"; case 413: return "Content Too Large";
    case 417: return "Expectation Failed"; case 431: return "Request Header Fields Too Large";
    case 501: return "Not Implemented"; case 507: return "Insufficient Storage";
    default: return "Internal Server Error";
    }
}

static int response(int status, uint32_t length)
{
    char header[192];
    int n = nn_snprintf(header, sizeof header,
        "HTTP/1.0 %d %s\r\nContent-Length: %u\r\nContent-Type: application/octet-stream\r\n"
        "Connection: close\r\nAllow: GET, PUT\r\n\r\n", status, reason(status), length);
    return net_send(HTTP_SOCKET, header, (uint32_t)n);
}

static int file_error(int e)
{
    if (e == SYS_ENOENT) return 404;
    if (e == SYS_ENOSPC || e == SYS_ENOMEM) return 507;
    if (e == SYS_EEXIST || e == SYS_EISDIR || e == SYS_ENOTDIR) return 409;
    return 500;
}

static void download(struct transfer *t)
{
    struct nn_stat st;
    int e = vfs_stat(t->req.path, &st);
    if (e || st.type != NN_TYPE_FILE) { response(e ? file_error(e) : 403, 0); return; }
    int fd = vfs_open(t->req.path, O_RDONLY);
    if (fd < 0) { response(file_error(fd), 0); return; }
    uint32_t left = st.size;
    if (response(200, left) >= 0) while (left) {
        uint32_t n = left < sizeof t->rx ? left : sizeof t->rx;
        e = vfs_read(fd, t->rx, n);
        if (e <= 0 || net_send(HTTP_SOCKET, t->rx, (uint32_t)e) < 0) break;
        left -= (uint32_t)e;
    }
    vfs_close(fd);
}

static void upload(struct transfer *t)
{
    struct nn_stat st;
    int e = vfs_stat(t->req.path, &st), existed = !e;
    if ((e && e != SYS_ENOENT) || (existed && st.type != NN_TYPE_FILE)) {
        response(e ? file_error(e) : 409, 0); return;
    }
    /* O_EXCL protects a pre-existing file, even after a reboot during a PUT.
     * A complete, closed temporary file is atomically renamed into place. */
    nn_snprintf(t->temp, sizeof t->temp, "/home/.http-%u.tmp", task_self()->pid);
    int fd = vfs_open(t->temp, O_WRONLY | O_CREAT | O_EXCL);
    if (fd < 0) { response(file_error(fd), 0); return; }
    if (t->req.expect) {
        static const char cont[] = "HTTP/1.1 100 Continue\r\n\r\n";
        if (net_send(HTTP_SOCKET, cont, sizeof cont - 1) < 0) e = SYS_EIO;
    }
    uint32_t left = t->req.length;
    t->deadline = jiffies + 30000u + left / 10u;
    if (e == SYS_ENOENT) e = 0;
    while (!e && left) {
        if (receive(t)) { e = SYS_EINTR; break; }
        uint32_t n = (uint32_t)(t->len - t->pos);
        if (n > left) n = left;
        int w = vfs_write(fd, t->rx + t->pos, n);
        if (w != (int)n) { e = w < 0 ? w : SYS_EIO; break; }
        t->pos += w; left -= n;
    }
    int closed = vfs_close(fd);
    if (!e) e = closed;
    if (!e) e = vfs_rename(t->temp, t->req.path);
    if (e) vfs_unlink(t->temp);
    response(e ? (e == SYS_EINTR ? 408 : file_error(e)) : (existed ? 204 : 201), 0);
}

static void serve(void *arg)
{
    struct transfer *t = arg;
    t->deadline = jiffies + 5000u;
    int e, bytes = 0;
    do {
        e = read_line(t);
        if (e < 0) break;
        bytes += e;
        if (bytes > 2048) { e = -431; break; }
        e = http_parse_line(&t->req, t->line);
    } while (!e);
    if (e < 0) response(-e, 0);
    else if (t->req.put) upload(t);
    else download(t);
    kfree(t);
    net_close(HTTP_SOCKET);
    /* Listen again at once (see diag.c: a reconnect in the gap waits 3 s). */
    if (net_services & NN_SVC_HTTPD) w5500_tcp_listen(HTTP_SOCKET, listener.port);
}

void httpd_poll(void)
{
    if (worker && worker->state == TASK_ZOMBIE) { task_join(worker, 0); worker = 0; }
    if (worker || !listener_poll(&listener, net_services & NN_SVC_HTTPD)) return;
    struct transfer *t = kmalloc(sizeof *t);
    if (t) {
        memset(t, 0, sizeof *t);
        worker = task_spawn("httpd", serve, t, 1152, 5, 0);   /* peak below 840 B measured */
    }
    if (!worker) { kfree(t); net_close(HTTP_SOCKET); }
}
