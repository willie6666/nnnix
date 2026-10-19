/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NNNIX_SHTEST_H
#define NNNIX_SHTEST_H
#include <stdint.h>
int sh_number(const char *s, int32_t *out);
int cmd_test(int argc, char **argv);
int cmd_true(int argc, char **argv);
int cmd_false(int argc, char **argv);
#endif
