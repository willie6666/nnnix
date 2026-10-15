/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NNNIX_INPUT_H
#define NNNIX_INPUT_H
#include <stdint.h>

/* CH9350 keyboard/mouse: UART1 bytes are parsed and keys are typed into tty0.
 * Runs inside the kernel daemon. */
void     input_poll(void);
uint32_t input_next_ms(void);         /* ms until a key auto-repeat is due; UINT32_MAX if none */
uint32_t input_mouse_reports(void);
uint32_t input_bad_bytes(void);

#endif
