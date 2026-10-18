/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NNNIX_EXEC_H
#define NNNIX_EXEC_H
#include "../lib/nnlib.h"
#include "../lib/sys.h"

extern const struct nn_lib nn_lib;

/* Start req->main (or, with image, a loaded program) as a child of the caller. */
struct loaded_image;
int spawn_child(const struct nn_spawn *req, struct loaded_image *image);
/* Load an NXE file into RAM and start it; pid or a negative error. */
int exec_program(const struct nn_exec *req);
/* pc lies in program text: APROM above the firmware (kernel/xip.c), or RAM
 * (where text goes when APROM has no room; the kernel never runs there). */
int exec_is_program_code(uint32_t pc);

#endif
