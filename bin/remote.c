/* SPDX-License-Identifier: GPL-3.0-or-later */
/* remote: the board as a small screen and keyboard for a computer.
 *   remote [-v] [PORT]
 * Waits for one TCP connection on PORT (default 1234), shows the NNV stream
 * that arrives on the LCD as it comes (tools/nnscreen.py: the computer's
 * screen), and sends the keyboard, mouse and keypad back on the same
 * connection (/dev/input events, 8 bytes each, for tools/nninput.py):
 *
 *   computer: nnscreen.py | nc BOARD 1234 | nninput.py
 *
 * Frames show when they have come, whatever time per frame the header gives;
 * sound records are skipped. While it runs the keyboard types nothing into the
 * console; the board's SW_INT button ends it (Ctrl-C too, from a telnet
 * session), and it ends when the computer closes the connection. -v prints
 * the frames shown, the bytes received and the events sent at the end.
 *
 * One task: the socket is read and /dev/input polled in the same loop, and the
 * stream is decoded a piece at a time into the one frame (about 1.2 KiB of
 * RAM besides the stack). */
#include "../user/cmds.h"
#include "../lib/stdio.h"
#include "../lib/string.h"
#include "../lib/sys.h"
#include "../lib/nnv.h"

#define PORT 1234u
#define BUF 128u                             /* one socket read */

enum { HEADER, RECORD, DATA };

struct remote {
    uint8_t frame[NNV_FRAME];
    uint8_t buf[BUF];
    uint8_t head[NNV_HEADER];                /* the stream's header, then each record's */
    uint8_t stage, kind;
    uint16_t got, pos;                       /* got: of head; pos: in a raw frame */
    uint32_t left;                           /* of the record's data */
    struct nnv_dec dec;
    int lcd;
    uint32_t frames, bytes, events;
};

/* The record has all its data: show the frame it made. 0, or -1 if damaged. */
static int record_end(struct remote *r)
{
    r->stage = RECORD;
    if (r->kind == NNV_DELTA && nnv_end(&r->dec)) return -1;
    if (r->kind != NNV_RAW && r->kind != NNV_DELTA) return 0;
    sys_write(r->lcd, r->frame, NNV_FRAME);
    r->frames++;
    return 0;
}

/* n more bytes of the stream: 0, or -1 when it is not NNV or damaged. */
static int feed(struct remote *r, const uint8_t *p, uint32_t n)
{
    while (n) {
        if (r->stage != DATA) {
            uint32_t want = r->stage == HEADER ? NNV_HEADER : NNV_RECORD;
            uint32_t k = want - r->got < n ? want - r->got : n;
            memcpy(r->head + r->got, p, k);
            r->got = (uint16_t)(r->got + k);
            p += k;
            n -= k;
            if (r->got < want) continue;
            r->got = 0;
            if (r->stage == HEADER) {
                struct nnv_info info;
                if (nnv_header(r->head, &info)) return -1;
                r->stage = RECORD;
                continue;
            }
            r->kind = r->head[0];
            r->left = nnv_u16(r->head + 1);
            if (r->kind == NNV_RAW && r->left != NNV_FRAME) return -1;
            nnv_start(&r->dec);
            r->pos = 0;
            r->stage = DATA;
            if (!r->left && record_end(r)) return -1;
            continue;
        }
        uint32_t k = r->left < n ? r->left : n;
        if (r->kind == NNV_RAW) {
            memcpy(r->frame + r->pos, p, k);
            r->pos = (uint16_t)(r->pos + k);
        } else if (r->kind == NNV_DELTA && nnv_feed(&r->dec, r->frame, p, k)) {
            return -1;
        }
        p += k;
        n -= k;
        r->left -= k;
        if (!r->left && record_end(r)) return -1;
    }
    return 0;
}

/* Events waiting in /dev/input go to the computer. 0, or -1 once there is no
 * more input (released) or the connection failed. */
static int send_events(struct remote *r, int input, int fd)
{
    struct nn_input_event ev[4];
    int n = sys_read(input, ev, sizeof ev);
    if (n == SYS_EAGAIN) return 0;
    if (n <= 0) return -1;
    if (sys_write(fd, ev, (size_t)n) < 0) return -1;
    r->events += (uint32_t)n / sizeof ev[0];
    return 0;
}

static int show(struct remote *r, int fd, int input)
{
    for (;;) {
        if (send_events(r, input, fd)) return 0;
        struct nn_sockinfo i;
        if (sys_sockinfo(fd, &i)) return 1;
        if (i.rx) {
            int n = sys_read(fd, r->buf, sizeof r->buf);
            if (n < 0) return 1;
            if (n == 0) return 0;
            r->bytes += (uint32_t)n;
            if (feed(r, r->buf, (uint32_t)n)) {
                printf("remote: %s\n", r->frames || r->stage != HEADER ? "damaged stream" : "not an NNV stream");
                return 1;
            }
            continue;
        }
        if (i.state != NN_SS_ESTABLISHED) return 0;     /* the computer closed */
        if (nn_interrupted()) return 130;
        sys_sleep(10);
    }
}

static int run(struct remote *r, uint16_t port, int verbose)
{
    int fd = sys_socket(NN_SOCK_TCP);
    if (fd < 0) { printf("remote: %s\n", errstr(fd)); return 1; }
    int rc = 1, e = sys_listen(fd, port), input = -1;
    if (e) { printf("remote: port %u: %s\n", port, errstr(e)); goto out; }
    printf("remote: waiting on port %u\n", port);
    struct nn_sockinfo i;
    for (;;) {
        sys_sockinfo(fd, &i);
        if (i.state == NN_SS_ESTABLISHED || i.state == NN_SS_CLOSE_WAIT) break;
        if (i.state == NN_SS_CLOSED) { printf("remote: the connection failed\n"); goto out; }
        if (nn_interrupted()) { rc = 130; goto out; }
        sys_sleep(20);
    }
    printf("remote: %u.%u.%u.%u connected; SW_INT ends\n", i.peer[0], i.peer[1], i.peer[2], i.peer[3]);
    r->lcd = sys_open("/dev/lcd", O_WRONLY);
    if (r->lcd < 0) { printf("remote: /dev/lcd: %s\n", errstr(r->lcd)); goto out; }
    input = sys_open("/dev/input", O_RDONLY | O_NONBLOCK);
    if (input < 0) { printf("remote: /dev/input: %s\n", errstr(input)); goto out; }
    uint32_t start = sys_uptime();
    rc = show(r, fd, input);
    uint32_t took = sys_uptime() - start;
    sys_close(input);
    input = -1;
    sys_close(r->lcd);                               /* the console text comes back */
    r->lcd = -1;
    if (verbose)
        printf("remote: %u frames, %u bytes in %u.%03u s, %u events sent\n", r->frames, r->bytes,
               took / 1000u, took % 1000u, r->events);
out:
    if (input >= 0) sys_close(input);
    if (r->lcd >= 0) sys_close(r->lcd);
    sys_close(fd);
    return rc;
}

int cmd_remote(int argc, char **argv)
{
    int verbose = 0, bad = 0;
    uint32_t port = PORT;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "-v")) { verbose = 1; continue; }
        uint32_t v = 0;
        const char *s = argv[i];
        for (; *s >= '0' && *s <= '9' && v < 65536u; ++s) v = v * 10u + (uint32_t)(*s - '0');
        if (*s || s == argv[i] || !v || v > 65535u) bad = 1;
        port = v;
    }
    if (bad) {
        printf("usage: remote [-v] [PORT]   (default port %u; on the computer: nnscreen.py | nc BOARD PORT | nninput.py)\n", PORT);
        return 2;
    }
    struct remote *r = sys_alloc(sizeof *r);
    if (!r) { printf("remote: %s\n", errstr(SYS_ENOMEM)); return 1; }
    memset(r, 0, sizeof *r);
    r->lcd = -1;
    (void)nn_interrupted();
    int rc = run(r, (uint16_t)port, verbose);
    sys_free(r);
    return rc;
}
