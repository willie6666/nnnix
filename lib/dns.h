/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NNNIX_DNS_H
#define NNNIX_DNS_H
#include <stdint.h>

/* A name to an IPv4 address: a dotted address as it is, otherwise an A record
 * asked of the DNS server in the network settings (ifconfig ... dns IP, or
 * DHCP's), over UDP, 3 tries of 2 s. CNAMEs in the answer are followed.
 * 0, or a negative error: SYS_ENOENT no such name (or no address),
 * SYS_ETIMEDOUT no answer, SYS_EINVAL no DNS server set, SYS_EINTR Ctrl-C. */
int dns_resolve(const char *name, uint8_t ip[4]);
int dns_parse_ip(const char *s, uint8_t ip[4]);   /* dotted quad: 0 or -1 */

#endif
