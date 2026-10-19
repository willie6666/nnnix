/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "shtest.h"
#include "../lib/stdio.h"
#include "../lib/string.h"
#include "../lib/sys.h"

int sh_number(const char *s, int32_t *out)
{
    int minus = *s == '-';
    if (*s == '-' || *s == '+') ++s;
    if (!*s) return -1;
    uint32_t n = 0, max = minus ? 2147483648u : 2147483647u;
    for (; *s; ++s) {
        if (*s < '0' || *s > '9' || n > (max - (unsigned)(*s - '0')) / 10u) return -1;
        n = n * 10u + (unsigned)(*s - '0');
    }
    *out = minus ? (int32_t)(0u - n) : (int32_t)n;
    return 0;
}

int cmd_true(int argc, char **argv) { (void)argc; (void)argv; return 0; }
int cmd_false(int argc, char **argv) { (void)argc; (void)argv; return 1; }

int cmd_test(int argc, char **argv)
{
    int n = argc - 1, negate = 0, yes = 0;
    char **a = argv + 1;
    if (!strcmp(argv[0], "[")) {
        if (!n || strcmp(a[n - 1], "]")) goto bad;
        --n;
    }
    if (n > 1 && !strcmp(a[0], "!")) { negate = 1; ++a; --n; }
    if (!n) yes = 0;
    else if (n == 1) yes = a[0][0] != 0;
    else if (n == 2) {
        if (!strcmp(a[0], "-n")) yes = a[1][0] != 0;
        else if (!strcmp(a[0], "-z")) yes = a[1][0] == 0;
        else if (!strcmp(a[0], "-e") || !strcmp(a[0], "-f") || !strcmp(a[0], "-d") || !strcmp(a[0], "-s")) {
            struct nn_stat st;
            int e = sys_stat(a[1], &st);
            yes = !e && (!strcmp(a[0], "-e") || (!strcmp(a[0], "-f") && st.type == NN_TYPE_FILE) ||
                         (!strcmp(a[0], "-d") && st.type == NN_TYPE_DIR) || (!strcmp(a[0], "-s") && st.size > 0));
        } else goto bad;
    } else if (n == 3) {
        if (!strcmp(a[1], "=") || !strcmp(a[1], "!=")) yes = (!strcmp(a[0], a[2])) == !strcmp(a[1], "=");
        else {
            int32_t x, y;
            if (sh_number(a[0], &x) || sh_number(a[2], &y)) goto bad;
            if (!strcmp(a[1], "-eq")) yes = x == y;
            else if (!strcmp(a[1], "-ne")) yes = x != y;
            else if (!strcmp(a[1], "-lt")) yes = x < y;
            else if (!strcmp(a[1], "-le")) yes = x <= y;
            else if (!strcmp(a[1], "-gt")) yes = x > y;
            else if (!strcmp(a[1], "-ge")) yes = x >= y;
            else goto bad;
        }
    } else goto bad;
    return (yes != negate) ? 0 : 1;
bad:
    printf("test: invalid expression (use strings, -n/-z, -e/-f/-d/-s, =/!=, -eq/-ne/-lt/-le/-gt/-ge)\n");
    return 2;
}
