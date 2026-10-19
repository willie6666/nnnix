/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../lib/dns.h"
#include "../user/cmds.h"
#include "../lib/stdio.h"
#include "../lib/sys.h"

int cmd_host(int argc, char **argv)
{
    if (argc != 2) { printf("usage: host NAME\n"); return 2; }
    uint8_t ip[4];
    int e = dns_resolve(argv[1], ip);
    if (e == SYS_ENOENT) { printf("host: %s: not found\n", argv[1]); return 1; }
    if (e == SYS_EINVAL) { printf("host: no DNS server (ifconfig eth0 ... dns IP, or dhcp)\n"); return 1; }
    if (e) { printf("host: %s: %s\n", argv[1], errstr(e)); return 1; }
    printf("%s has address %u.%u.%u.%u\n", argv[1], ip[0], ip[1], ip[2], ip[3]);
    return 0;
}
