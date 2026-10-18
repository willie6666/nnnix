/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Clock and network commands for nsh. They use only system calls. */
#include "cmds.h"
#include "../lib/stdio.h"
#include "../lib/string.h"
#include "../lib/sys.h"
#include "../lib/time.h"

#define NTP_UNIX_OFFSET 2208988800u  /* seconds from 1900 to 1970 */

static int usage(const char *text) { printf("usage: %s\n", text); return 1; }

/* "a.b.c.d" with nothing after it. */
static int parse_ip(const char *s, uint8_t ip[4])
{
    for (int i = 0; i < 4; ++i) {
        unsigned v = 0, digits = 0;
        while (*s >= '0' && *s <= '9' && digits < 4) { v = v * 10u + (unsigned)(*s++ - '0'); ++digits; }
        if (!digits || v > 255 || *s != (i < 3 ? '.' : 0)) return -1;
        ip[i] = (uint8_t)v;
        if (i < 3) ++s;
    }
    return 0;
}

static int parse_port(const char *s, uint16_t *port)
{
    unsigned v = 0;
    if (!*s) return -1;
    for (; *s; ++s) {
        if (*s < '0' || *s > '9' || v > 6553u) return -1;
        v = v * 10u + (unsigned)(*s - '0');
    }
    if (!v || v > 65535u) return -1;
    *port = (uint16_t)v;
    return 0;
}

static void print_ip(const uint8_t ip[4]) { printf("%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]); }

/* ---- date ---- */

/* Local time offset in seconds from /etc/tz: "+8", "-5", "+05:30" or "+0530". */
static int32_t tz_offset(void)
{
    char b[16];
    int fd = sys_open("/etc/tz", O_RDONLY);
    if (fd < 0) return 0;
    int n = sys_read(fd, b, sizeof b - 1);
    sys_close(fd);
    if (n <= 0) return 0;
    b[n] = 0;
    const char *p = b;
    int sign = 1;
    if (*p == '+' || *p == '-') sign = *p++ == '-' ? -1 : 1;
    int32_t digits = 0, count = 0, minutes = 0;
    while (*p >= '0' && *p <= '9' && count < 4) { digits = digits * 10 + (*p++ - '0'); ++count; }
    if (*p == ':' && count <= 2) {
        ++p;
        while (*p >= '0' && *p <= '9') minutes = minutes * 10 + (*p++ - '0');
    } else if (count > 2) {
        minutes = digits % 100;
        digits /= 100;
    }
    if (!count || digits > 14 || minutes > 59) return 0;
    return sign * (digits * 3600 + minutes * 60);
}

static void print_time(uint32_t t, int32_t tz)
{
    char buf[20];
    nn_format_time(t + (uint32_t)tz, buf);
    if (!tz) { printf("%s UTC\n", buf); return; }
    int32_t a = tz < 0 ? -tz : tz;
    printf("%s %c%02d%02d\n", buf, tz < 0 ? '-' : '+', (int)(a / 3600), (int)(a / 60 % 60));
}

int cmd_date(int argc, char **argv)
{
    int32_t tz = tz_offset();
    int i = 1;
    if (i < argc && !strcmp(argv[i], "-u")) { tz = 0; ++i; }
    if (i == argc) { print_time(sys_time(), tz); return 0; }
    if (i + 1 == argc && !strcmp(argv[i], "+%s")) { printf("%u\n", sys_time()); return 0; }
    if (strcmp(argv[i], "-s") || i + 1 == argc || i + 3 < argc)
        return usage("date [-u] [+%s | -s 'YYYY-MM-DD HH:MM[:SS]']");
    char text[24];
    uint32_t n = (uint32_t)strlen(argv[i + 1]);
    if (n >= sizeof text) return usage("date -s 'YYYY-MM-DD HH:MM[:SS]'");
    strcpy(text, argv[i + 1]);
    if (i + 2 < argc) {                                /* date -s 2026-10-16 12:00 */
        if (n + 1 + strlen(argv[i + 2]) >= sizeof text) return usage("date -s 'YYYY-MM-DD HH:MM[:SS]'");
        text[n] = ' ';
        strcpy(text + n + 1, argv[i + 2]);
    }
    uint32_t t;
    if (nn_parse_time(text, &t)) { printf("date: invalid time '%s'\n", text); return 1; }
    t -= (uint32_t)tz;
    int e = sys_stime(t);
    if (e) { printf("date: %s\n", errstr(e)); return 1; }
    print_time(t, tz);
    return 0;
}

/* ---- interface configuration ---- */

static unsigned prefix_of(const uint8_t m[4])
{
    unsigned n = 0;
    for (int i = 0; i < 4; ++i) for (int b = 7; b >= 0 && (m[i] >> b & 1u); --b) ++n;
    return n;
}

static void mask_of(unsigned prefix, uint8_t m[4])
{
    for (int i = 0; i < 4; ++i) {
        unsigned bits = prefix > 8 ? 8 : prefix;
        m[i] = (uint8_t)(0xff00u >> bits);
        prefix -= bits;
    }
}

static const char *const dhcp_states[] = { "off", "selecting", "requesting", "bound", "renewing", "failed" };

static void show_interface(const struct nn_netcfg *c)
{
    printf("eth0: ");
    if (!(c->link & 8u)) printf("no chip\n");
    else if (!(c->link & 1u)) printf("link down\n");
    else printf("up %s %s\n", (c->link & 2u) ? "100M" : "10M", (c->link & 4u) ? "full" : "half");
    printf(" inet "); print_ip(c->ip); printf("/%u\n", prefix_of(c->mask));
    printf(" gw "); print_ip(c->gw); printf("\n dns "); print_ip(c->dns);
    printf("\n mac %02x:%02x:%02x:%02x:%02x:%02x\n", c->mac[0], c->mac[1], c->mac[2], c->mac[3], c->mac[4], c->mac[5]);
    printf(" dhcp %s", dhcp_states[c->dhcp < 6 ? c->dhcp : 0]);
    if (c->dhcp == NN_DHCP_BOUND || c->dhcp == NN_DHCP_RENEWING) {
        printf(" from "); print_ip(c->server); printf(", %u s left", c->lease);
    }
    printf("\n");
}

int cmd_ifconfig(int argc, char **argv)
{
    static const char use[] = "ifconfig [eth0] [IP/PREFIX [gw IP] [dns IP]]";
    struct nn_netcfg c;
    sys_netcfg(&c, NN_NET_GET);
    int i = 1;
    if (i < argc && !strcmp(argv[i], "eth0")) ++i;
    if (i == argc) { show_interface(&c); return 0; }
    char addr[20];
    if (strlen(argv[i]) >= sizeof addr) return usage(use);
    strcpy(addr, argv[i++]);
    char *slash = strchr(addr, '/');
    unsigned prefix = 24;
    if (slash) {
        *slash++ = 0;
        prefix = 0;
        if (!*slash || strlen(slash) > 2) return usage(use);
        for (const char *p = slash; *p; ++p) {
            if (*p < '0' || *p > '9') return usage(use);
            prefix = prefix * 10u + (unsigned)(*p - '0');
        }
        if (prefix > 32) return usage(use);
    }
    if (parse_ip(addr, c.ip)) return usage(use);
    mask_of(prefix, c.mask);
    memset(c.gw, 0, 4);
    for (; i + 1 < argc; i += 2) {
        uint8_t *dst = !strcmp(argv[i], "gw") ? c.gw : !strcmp(argv[i], "dns") ? c.dns : 0;
        if (!dst || parse_ip(argv[i + 1], dst)) return usage(use);
    }
    if (i != argc) return usage(use);
    int e = sys_netcfg(&c, NN_NET_SET);
    if (e) { printf("ifconfig: %s\n", errstr(e)); return 1; }
    return 0;
}

int cmd_dhcp(int argc, char **argv)
{
    uint32_t wait_s = 20;
    if (argc == 3 && !strcmp(argv[1], "-t")) {
        wait_s = 0;
        for (const char *p = argv[2]; *p; ++p) {
            if (*p < '0' || *p > '9' || wait_s > 3600) return usage("dhcp [-t SECONDS]");
            wait_s = wait_s * 10u + (uint32_t)(*p - '0');
        }
    } else if (argc != 1) return usage("dhcp [-t SECONDS]");
    struct nn_netcfg c;
    (void)nn_interrupted();
    uint32_t start = sys_uptime();
    /* At boot /etc/rc may run before the chip is found and the link is up. */
    while (sys_netcfg(&c, NN_NET_GET), !(c.link & 1u)) {
        if (sys_uptime() - start >= wait_s * 1000u) { printf("dhcp: no link\n"); return 1; }
        if (nn_interrupted()) return 130;
        sys_sleep(100);
    }
    int e = sys_netcfg(&c, NN_NET_DHCP);
    if (e) { printf("dhcp: %s\n", errstr(e)); return 1; }
    while (sys_uptime() - start < wait_s * 1000u) {
        sys_sleep(100);
        sys_netcfg(&c, NN_NET_GET);
        if (c.dhcp == NN_DHCP_BOUND) {
            printf("dhcp: "); print_ip(c.ip); printf("/%u from ", prefix_of(c.mask));
            print_ip(c.server); printf(", lease %u s\n", c.lease);
            return 0;
        }
        if (c.dhcp == NN_DHCP_FAILED) {
            int none = !(c.ip[0] | c.ip[1] | c.ip[2] | c.ip[3]);
            printf("dhcp: no answer, %s\n", none ? "no address (still trying)" : "address unchanged");
            return 1;
        }
        if (nn_interrupted()) { printf("dhcp: still trying in the background\n"); return 130; }
    }
    printf("dhcp: no lease yet, still trying in the background\n");
    return 1;
}

int cmd_service(int argc, char **argv)
{
    static const char *const names[] = { "telnetd", "httpd" };
    struct nn_netcfg c;
    sys_netcfg(&c, NN_NET_GET);
    if (argc == 1) {
        for (int i = 0; i < 2; ++i) printf("%-8s %s\n", names[i], (c.services >> i & 1u) ? "on" : "off");
        return 0;
    }
    int i = argc == 3 ? (!strcmp(argv[1], "telnetd") ? 0 : !strcmp(argv[1], "httpd") ? 1 : -1) : -1;
    int on = argc == 3 ? (!strcmp(argv[2], "start") ? 1 : !strcmp(argv[2], "stop") ? 0 : -1) : -1;
    if (i < 0 || on < 0) return usage("service [telnetd|httpd start|stop]");
    if (on) c.services |= (uint8_t)(1u << i);
    else c.services &= (uint8_t)~(1u << i);
    return sys_netcfg(&c, NN_NET_SERVICES) ? 1 : 0;
}

static const char *state_name(uint8_t s)
{
    switch (s) {
    case 0x00: return "CLOSED";
    case 0x13: return "INIT";
    case 0x14: return "LISTEN";
    case 0x15: return "SYNSENT";
    case 0x16: return "SYNRECV";
    case 0x17: return "ESTAB";
    case 0x18: return "FINWAIT";
    case 0x1a: return "CLOSING";
    case 0x1b: return "TIMEWAIT";
    case 0x1c: return "CLOSEWAIT";
    case 0x1d: return "LASTACK";
    case 0x22: return "UDP";
    default: return "?";
    }
}

int cmd_netstat(int argc, char **argv)
{
    static const char *const owners[8] = { "diag", "telnet", "telnet", "telnet", "http", "user", "user", "dhcp" };
    (void)argc; (void)argv;
    printf("S USE    PROTO PORT  STATE     PEER\n");
    for (int sn = 0; sn < 8; ++sn) {
        struct nn_sockinfo i;
        if (sys_sockinfo(-1 - sn, &i)) continue;
        printf("%d %-6s %-5s %-5u %-9s", sn, owners[sn], i.proto == NN_SOCK_TCP ? "tcp" : i.proto == NN_SOCK_UDP ? "udp" : "-",
               i.proto ? i.port : 0, state_name(i.state));
        if (i.state >= 0x15 && i.state <= 0x1d) { printf(" "); print_ip(i.peer); printf(":%u", i.peer_port); }
        printf("\n");
    }
    return 0;
}

/* ---- nc ---- */

static int connected(uint8_t s) { return s == NN_SS_ESTABLISHED || s == NN_SS_CLOSE_WAIT; }

/* Standard input (typed lines, or a file or pipe) goes to the socket (fd 1)
 * while the parent prints what arrives, so neither direction waits for the other. */
static int nc_typist(int argc, char **argv)
{
    char buf[96];
    int n;
    (void)argc; (void)argv;
    while ((n = sys_read(0, buf, sizeof buf)) > 0) {
        int w = sys_write(1, buf, (size_t)n);
        if (w == SYS_EINVAL) { printf("nc: no peer yet\n"); continue; }
        if (w < 0) return 1;
    }
    return 0;
}

/* Socket to stdout until the peer closes, Ctrl-C, idle_ms of silence, or (with
 * until_child) the typist ends. */
static int nc_pump(int fd, int proto, int child, int until_child, uint32_t idle_ms)
{
    char buf[128];
    uint32_t last = sys_uptime();
    for (;;) {
        struct nn_sockinfo i;
        if (sys_sockinfo(fd, &i)) return 1;
        if (i.rx) {
            int n = sys_read(fd, buf, sizeof buf);
            if (n == SYS_EAGAIN) continue;             /* a stranger's datagram, dropped */
            if (n < 0) return 1;
            if (n > 0 && nn_write_all(1, buf, (size_t)n) < 0) return 1;
            if (n == 0 && proto == NN_SOCK_TCP) return 0;
            last = sys_uptime();
            continue;
        }
        if (proto == NN_SOCK_TCP && i.state != NN_SS_ESTABLISHED) return 0;     /* peer closed */
        int status;
        if (child > 0 && sys_wait(child, &status, 0) == child) {
            child = 0;
            if (until_child) return 0;                 /* ^D or ^C typed */
            last = sys_uptime();                       /* all sent: the silence counts from here */
        }
        if (!until_child && nn_interrupted()) return 130;
        if (idle_ms && !child && sys_uptime() - last >= idle_ms) return 0;
        sys_sleep(20);
    }
}

int cmd_nc(int argc, char **argv)
{
    static const char use[] = "nc [-u] HOST PORT | nc [-u] -l PORT";
    int proto = NN_SOCK_TCP, listen = 0, i = 1;
    for (; i < argc && argv[i][0] == '-'; ++i) {
        if (!strcmp(argv[i], "-u")) proto = NN_SOCK_UDP;
        else if (!strcmp(argv[i], "-l")) listen = 1;
        else return usage(use);
    }
    struct nn_sockaddr a;
    memset(&a, 0, sizeof a);
    if (listen ? i + 1 != argc || parse_port(argv[i], &a.port)
               : i + 2 != argc || parse_ip(argv[i], a.ip) || parse_port(argv[i + 1], &a.port))
        return usage(use);
    (void)nn_interrupted();
    int fd = sys_socket(proto);
    if (fd < 0) { printf("nc: %s\n", errstr(fd)); return 1; }
    int e = listen ? sys_listen(fd, a.port) : sys_connect(fd, &a);
    int rc = 1;
    if (e) { printf("nc: %s\n", errstr(e)); goto out; }
    if (proto == NN_SOCK_TCP) {                        /* wait for the connection */
        for (;;) {
            struct nn_sockinfo info;
            sys_sockinfo(fd, &info);
            if (connected(info.state)) break;
            if (info.state == NN_SS_CLOSED) { printf("nc: connection refused or timed out\n"); goto out; }
            if (nn_interrupted()) { rc = 130; goto out; }
            sys_sleep(20);
        }
    }
    int tty = sys_isatty(0) == 1;
    char *child_argv[] = { "nc", 0 };
    /* Terminal: the session ends with ^D/^C. File or pipe: once it is all sent,
     * keep printing until the peer closes (UDP: 2 s of silence). */
    struct nn_spawn sp = { nc_typist, child_argv, 1, tty ? -1 : 0, fd, 1024 };
    int pid = sys_spawn(&sp);
    if (pid < 0) { printf("nc: %s\n", errstr(pid)); goto out; }
    rc = nc_pump(fd, proto, pid, tty, !tty && proto == NN_SOCK_UDP ? 2000u : 0);
    int status;
    sys_kill(pid);
    sys_wait(pid, &status, NN_FOREVER);
out:
    sys_close(fd);
    return rc;
}

/* ---- ntpdate ---- */

int cmd_ntpdate(int argc, char **argv)
{
    struct nn_sockaddr a = { .port = 123 };
    if (argc != 2 || parse_ip(argv[1], a.ip)) return usage("ntpdate SERVER-IP");
    int fd = sys_socket(NN_SOCK_UDP);
    if (fd < 0) { printf("ntpdate: %s\n", errstr(fd)); return 1; }
    int rc = 1, e = sys_connect(fd, &a);
    (void)nn_interrupted();
    for (int attempt = 0; !e && attempt < 3; ++attempt) {
        uint8_t p[48];
        /* Our transmit time, which the reply must give back as its originate
         * time: any number will do, one hard to guess is better. */
        uint32_t nonce[2] = { sys_uptime() * 2654435761u ^ (uint32_t)fd, sys_time() ^ (uint32_t)attempt << 16 };
        memset(p, 0, sizeof p);
        p[0] = 0x23;                                   /* version 4, client */
        memcpy(p + 40, nonce, sizeof nonce);
        e = sys_write(fd, p, sizeof p);
        if (e < 0) break;
        e = 0;
        for (uint32_t start = sys_uptime(); sys_uptime() - start < 2000u;) {
            struct nn_sockinfo i;
            sys_sockinfo(fd, &i);
            if (!i.rx) {
                if (nn_interrupted()) { rc = 130; goto out; }
                sys_sleep(20);
                continue;
            }
            /* From the server (the socket drops others), a server's answer
             * (mode 4, version 3 or 4, stratum 1-15) to this request. */
            if (sys_read(fd, p, sizeof p) != sizeof p || (p[0] & 7u) != 4u || (p[0] >> 3 & 7u) < 3u ||
                (p[0] >> 3 & 7u) > 4u || !p[1] || p[1] > 15u || memcmp(p + 24, nonce, sizeof nonce)) continue;
            uint32_t secs = (uint32_t)p[40] << 24 | (uint32_t)p[41] << 16 | (uint32_t)p[42] << 8 | p[43];
            uint32_t t = secs - NTP_UNIX_OFFSET + (p[44] >= 0x80u ? 1u : 0u);
            int32_t delta = (int32_t)(t - sys_time());
            e = sys_stime(t);
            if (e) break;
            print_time(t, tz_offset());
            printf("ntpdate: offset %d s\n", (int)delta);
            rc = 0;
            goto out;
        }
    }
    printf("ntpdate: %s\n", e ? errstr(e) : "no answer");
out:
    sys_close(fd);
    return rc;
}
