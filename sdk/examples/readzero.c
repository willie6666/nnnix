/* SPDX-License-Identifier: GPL-3.0-or-later */
/* readzero: a read of 0 bytes returns 0 at once, from the terminal and from
 * an empty pipe whose writer is still open (a test). */
#include "nnnix.h"

int main(void)
{
    char c;
    int fds[2], bad = 0;
    uint32_t t0 = sys_uptime();
    if (sys_read(0, &c, 0) != 0) bad = 1;            /* the terminal, or whatever stdin is */
    if (sys_pipe(fds) < 0) { puts("readzero: no pipe"); return 1; }
    if (sys_read(fds[0], &c, 0) != 0) bad = 1;       /* empty, writer open: must not wait */
    sys_close(fds[0]);
    sys_close(fds[1]);
    uint32_t ms = sys_uptime() - t0;
    printf("readzero: %s in %u ms\n", bad ? "FAIL" : "ok", ms);
    return bad;
}
