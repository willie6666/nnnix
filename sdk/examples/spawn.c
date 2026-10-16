/* SPDX-License-Identifier: GPL-3.0-or-later */
/* spawn [wait]: start a worker task (a function of this program) that
 * computes for a while; with "wait", wait for it, otherwise return at once.
 * The worker shares this program's RAM image, which stays until both end. */
#include "nnnix.h"

static volatile uint32_t sink;

static int worker(int argc, char **argv)
{
    (void)argc; (void)argv;
    for (uint32_t i = 0; i < 12000000u; ++i) sink += i;   /* about 2 s, no system calls */
    printf("worker done\n");          /* if the parent has ended, the worker ends here */
    return 0;
}

int main(int argc, char **argv)
{
    char *name[] = { "worker", 0 };
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
