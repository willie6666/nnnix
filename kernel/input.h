/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NNNIX_INPUT_H
#define NNNIX_INPUT_H
#include <stdint.h>

/* CH9350 keyboard/mouse and the board's keys (kernel/input.c). UART1 bytes
 * are parsed in the kernel daemon; keys are typed into tty0, or, while a
 * program holds /dev/input, become its events (lib/sys.h, struct
 * nn_input_event), with the mouse and the 1-9 keypad. SW_INT ends the program
 * holding /dev/input, or else the one holding /dev/lcd. */
void     input_init(void);            /* the board's key pins */
void     input_poll(void);
uint32_t input_next_ms(void);         /* ms until a key repeat or a board key scan is due; UINT32_MAX if none */
uint32_t input_mouse_reports(void);
uint32_t input_bad_bytes(void);

/* /dev/input: one holder at a time (SYS_EBUSY), which is the calling task. */
int      input_claim(void);
void     input_release(void);
uint16_t input_owner(void);           /* the holder's pid, 0 if none */
/* Whole events into buf: bytes, 0 if released meanwhile, SYS_EAGAIN (nonblock
 * and none waiting), SYS_EINTR (killed), SYS_EINVAL (n under one event). */
int      input_read(void *buf, uint32_t n, int nonblock);

/* Tests (diag): CH9350 frames as if from UART1 (the bytes that fitted in its buffer),
 * and board keys held down (bits 0-8 keys 1-9, bit 9 SW_INT; 0 releases). */
uint32_t input_feed(const uint8_t *p, uint32_t n);
void     input_inject_keys(uint16_t mask);

#endif
