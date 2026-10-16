/* SPDX-License-Identifier: GPL-3.0-or-later */
/* spawn [wait|share]: start a worker task (a function of this program) that
 * computes for a while; with "wait", wait for it, otherwise return at once.
 * The worker shares this program's RAM image, which stays until both end;
 * with "share" it also keeps writing into a malloc'd buffer of this program,
 * which must stay allocated while the worker runs. */
#include "nnnix.h"

static volatile uint32_t sink;
static volatile uint8_t *shared;
#define SHARED 512u                               /* a power of two: masked, no division */

static int worker(int argc, char **argv)
{
    (void)argc; (void)argv;
    volatile uint8_t *buf = shared;
    for (uint32_t i = 0; i < 12000000u; ++i) {     /* about 2 s, no system calls */
        sink += i;
        if (buf) buf[i & (SHARED - 1u)] = 0xff;
    }
    printf("worker done\n");          /* if the parent has ended, the worker ends here */
    return 0;
}

int main(int argc, char **argv)
{
    char *name[] = { "worker", 0 };
    if (argc > 1 && !strcmp(argv[1], "share")) {
        shared = malloc(SHARED);
        if (!shared) { printf("spawn: %s\n", errstr(SYS_ENOMEM)); return 1; }
    }
    struct nn_spawn s = { worker, name, 1, -1, -1, 768 };
    int pid = sys_spawn(&s);
    if (pid < 0) { printf("spawn: %s\n", errstr(pid)); return 1; }
    printf("worker %d started\n", pid);
    if (argc > 1 && !strcmp(argv[1], "wait")) {
        int status;
        sys_wait(pid, &status, NN_FOREVER);
    }
    (void)argv;
    return 0;
}
