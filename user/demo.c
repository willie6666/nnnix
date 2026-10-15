/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Scheduler demonstration programs. They use only the system-call interface. */
#include "demo.h"
#include "../lib/sys.h"
#include "../lib/fmt.h"

/* Spin until the given uptime, counting loop iterations; exit code = loops. */
void demo_hog(void *arg)
{
    uint32_t until = (uint32_t)(uintptr_t)arg;
    int32_t loops = 0;
    while ((int32_t)(sys_uptime() - until) < 0) loops++;
    sys_exit(loops);
}

/* Sleep 10 ms `periods` times; exit code = worst wake-up lateness in ms. */
void demo_ticker(void *arg)
{
    uint32_t periods = (uint32_t)(uintptr_t)arg, worst = 0;
    char line[48];
    int n = nn_snprintf(line, sizeof line, "ticker: pid %d, %u periods\n", sys_getpid(), periods);
    sys_write(1, line, (size_t)n);
    for (uint32_t i = 0; i < periods; ++i) {
        uint32_t target = sys_uptime() + 10u;
        sys_sleep(10);
        uint32_t late = sys_uptime() - target;
        if (late > worst) worst = late;
    }
    sys_exit((int)worst);
}

/* Recurse until the kernel notices the stack running into its red zone. */
static int __attribute__((noinline)) dive(int depth)
{
    volatile unsigned char pad[40];
    pad[0] = (unsigned char)depth;
    sys_sleep(1);
    if (depth > 10000) return 0;
    return dive(depth + 1) + pad[0];
}

void demo_overflow(void *arg)
{
    (void)arg;
    sys_exit(dive(0));
}
