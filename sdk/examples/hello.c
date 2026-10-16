/* SPDX-License-Identifier: GPL-3.0-or-later */
/* hello [ARG...]: greets, lists its arguments, exits with their number. */
#include "nnnix.h"

static int calls;                     /* bss */
static const char *greeting = "hello from a loaded program";   /* data: a relocated pointer */

int main(int argc, char **argv)
{
    calls++;
    printf("%s, pid %d\n", greeting, sys_getpid());
    for (int i = 1; i < argc; ++i) printf("argv[%d] = %s\n", i, argv[i]);
    return argc - 1 + calls - 1;
}
