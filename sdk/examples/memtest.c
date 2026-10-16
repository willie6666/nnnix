/* SPDX-License-Identifier: GPL-3.0-or-later */
/* memtest [MS]: fill heap blocks with a pattern, wait MS milliseconds (default
 * 3000), and check that nothing else wrote into them. */
#include "nnnix.h"

#define BLOCK 256u
#define BLOCKS 16

int main(int argc, char **argv)
{
    uint32_t ms = 3000;
    if (argc > 1) {
        ms = 0;
        for (const char *p = argv[1]; *p >= '0' && *p <= '9'; ++p) ms = ms * 10u + (uint32_t)(*p - '0');
    }
    uint8_t *b[BLOCKS];
    int n = 0;
    for (; n < BLOCKS && (b[n] = malloc(BLOCK)); ++n)
        for (uint32_t i = 0; i < BLOCK; ++i) b[n][i] = (uint8_t)(i * 7u + (uint32_t)n);
    sys_sleep(ms);
    int bad = 0;
    for (int k = 0; k < n; ++k)
        for (uint32_t i = 0; i < BLOCK; ++i)
            if (b[k][i] != (uint8_t)(i * 7u + (uint32_t)k)) { ++bad; break; }
    printf("memtest: %d blocks of %u B, %s\n", n, BLOCK, bad ? "CORRUPTED" : "intact");
    return bad != 0;
}
