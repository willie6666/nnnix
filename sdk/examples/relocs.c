/* SPDX-License-Identifier: GPL-3.0-or-later */
/* relocs: every kind of address a program can hold, checked as it runs (a
 * test). Tables in text pointing into text, data pointing into text, data and
 * bss, and data that must start afresh in every run although the text is
 * shared. The tag lets a test make copies that differ (tools/nntest.py). */
#include "nnnix.h"

static const char tag[] = "TAG:00000000";
static int counter = 5;                                   /* data */
static int zeroed[4];                                     /* bss */
static int twice(int x) { return 2 * x; }
static int thrice(int x) { return 3 * x; }
static const char *const names[] = { "alpha", "beta" };   /* text -> text */
static int (*const ops[])(int) = { twice, thrice };       /* text -> text */
static int *pointers[] = { &counter, zeroed };            /* data -> data, bss */
static int (*hook)(int) = thrice;                         /* data -> text */
static const char *greeting = "hello";                    /* data -> text */
static volatile int zero = 0, one = 1;                    /* indexes the compiler cannot fold */

int main(int argc, char **argv)
{
    int bad = 0;
    if (strcmp(names[one], "beta") || strcmp(names[zero], "alpha")) bad |= 1;
    if (ops[zero](21) != 42 || ops[one](2) != 6) bad |= 2;
    if (*pointers[zero] != 5 || counter != 5) bad |= 4;   /* each run starts from the file's data */
    pointers[one][2] = 7;
    if (zeroed[2] != 7 || zeroed[0] || zeroed[3]) bad |= 8;
    if (hook(4) != 12) bad |= 16;
    if (strcmp(greeting, "hello")) bad |= 32;
    counter++;
    pointers[zero] = 0;                                   /* the next run must not see this */
    hook = twice;
    if (argc > 1) sys_sleep((uint32_t)(argv[1][0] - '0') * 1000u);   /* stay a while: "relocs 3" */
    printf("relocs %s: %s (%d)\n", tag + 4, bad ? "FAIL" : "ok", bad);
    return bad;
}
