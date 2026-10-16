/* SPDX-License-Identifier: GPL-3.0-or-later */
/* upper: copy standard input to standard output in upper case (for pipes). */
#include "nnnix.h"

static char buf[64];

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    int n;
    while ((n = sys_read(0, buf, sizeof buf)) > 0) {
        for (int i = 0; i < n; ++i) if (buf[i] >= 'a' && buf[i] <= 'z') buf[i] = (char)(buf[i] - 32);
        if (sys_write(1, buf, (size_t)n) < 0) return 1;
    }
    return n < 0 ? 1 : 0;
}
