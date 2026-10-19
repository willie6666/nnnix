/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NNNIX_SHFLOW_H
#define NNNIX_SHFLOW_H
#include <stdint.h>

#define SH_BLOCKS 8
struct sh_block { uint32_t offset; int result; uint8_t state, flags; };
struct sh_flow { struct sh_block blocks[SH_BLOCKS]; unsigned depth; };
/* Each script owns the blocks above base; a sourced file cannot close or
 * break its caller's blocks. run returns 1 for exit, writes command status.
 * Conditions may not start another script or exit the shell. */
struct sh_flow_ops {
    int (*run)(void *ctx, const char *command, int condition);
    int (*seek)(void *ctx, uint32_t offset);
};
const char *sh_flow_line(struct sh_flow *f, unsigned base, char *line, uint32_t offset,
                         int *status, const struct sh_flow_ops *ops, void *ctx, int *leave);
void sh_flow_record(struct sh_flow *f, unsigned base, int status);
int sh_control_word(const char *line);

#endif
