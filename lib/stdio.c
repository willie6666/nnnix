/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdarg.h>
#include "stdio.h"
#include "fmt.h"
#include "string.h"
#include "sys.h"

int printf(const char *fmt, ...)
{
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    int n = nn_vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof buf - 1) n = sizeof buf - 1;
    return sys_write(1, buf, (size_t)n);
}

int fputs_fd(const char *s, int fd) { return sys_write(fd, s, strlen(s)); }
