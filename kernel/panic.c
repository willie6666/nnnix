/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdarg.h>
#include "panic.h"
#include "../lib/fmt.h"
#include "../arch/nuc140/arch.h"

void panic(const char *fmt, ...)
{
    char msg[48];
    va_list ap;
    irq_disable();
    va_start(ap, fmt);
    nn_vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    arch_panic(msg, (uint32_t)(uintptr_t)__builtin_return_address(0));
}
