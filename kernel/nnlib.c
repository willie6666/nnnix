/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The library table handed to loaded programs (lib/nnlib.h). */
#include "../lib/nnlib.h"
#include "../lib/fmt.h"
#include "../lib/stdio.h"
#include "../lib/string.h"
#include "../user/cmds.h"
#include "../drivers/font3x5.h"
#include "../drivers/font5x7.h"

uint32_t __aeabi_uidiv(uint32_t n, uint32_t d);
uint64_t __aeabi_uidivmod(uint32_t n, uint32_t d);
int32_t  __aeabi_idiv(int32_t n, int32_t d);
uint64_t __aeabi_idivmod(int32_t n, int32_t d);

const struct nn_lib nn_lib = {
    .magic = NN_LIB_MAGIC, .version = NN_LIB_VERSION, .size = sizeof(struct nn_lib),
    .vprintf = vprintf, .vsnprintf = nn_vsnprintf,
    .memcpy = memcpy, .memmove = memmove, .memset = memset, .memcmp = memcmp,
    .strlen = strlen, .strcmp = strcmp, .strncmp = strncmp, .strcpy = strcpy, .strchr = strchr,
    .uidiv = __aeabi_uidiv, .uidivmod = __aeabi_uidivmod, .idiv = __aeabi_idiv, .idivmod = __aeabi_idivmod,
    .errstr = errstr,
    .gmtime = nn_gmtime, .mktime = nn_mktime, .format_time = nn_format_time,
    .font3x5 = font3x5, .font5x7 = font5x7,
};
