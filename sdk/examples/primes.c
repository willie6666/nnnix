/* SPDX-License-Identifier: GPL-3.0-or-later */
/* primes [LIMIT]: count the primes up to LIMIT (default 10000) with a sieve
 * in heap memory; the division helpers and malloc come from the firmware. */
#include "nnnix.h"

int main(int argc, char **argv)
{
    uint32_t limit = 10000;
    if (argc > 1) {
        limit = 0;
        for (const char *p = argv[1]; *p; ++p) {
            if (*p < '0' || *p > '9' || limit > 100000) { printf("usage: primes [LIMIT up to 100000]\n"); return 2; }
            limit = limit * 10u + (uint32_t)(*p - '0');
        }
    }
    uint8_t *odd = malloc(limit / 16u + 1u);  /* one bit per odd number */
    if (!odd) { printf("primes: %s\n", errstr(SYS_ENOMEM)); return 1; }
    memset(odd, 0, limit / 16u + 1u);
    uint32_t count = limit >= 2, last = limit >= 2 ? 2 : 0, start = sys_uptime();
    for (uint32_t n = 3; n <= limit; n += 2) {
        uint32_t i = n / 2u;
        if (odd[i / 8u] & (1u << (i % 8u))) continue;
        count++;
        last = n;
        for (uint32_t m = n * n; m <= limit && m >= n; m += 2u * n) odd[(m / 2u) / 8u] |= (uint8_t)(1u << ((m / 2u) % 8u));
    }
    printf("%u primes up to %u, the largest %u (%u ms)\n", count, limit, last, sys_uptime() - start);
    return 0;
}
