/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NNNIX_DIAG_H
#define NNNIX_DIAG_H
#include <stdint.h>

/* Diagnostic TCP service on W5500 socket 0, port 5000 (used by tools/nntest.py).
 * Request: cmd (1 byte), SPI divider (1 byte), length (u32 big endian).
 *   'P' banner text          'S' status text            'L' kernel log ring
 *   'K' task list (ps)
 *   'T' send length pattern bytes
 *   'R' receive length pattern bytes, reply u32 BE: length, errors, ms
 *   'U' CH376S write/read/verify/delete of length bytes, text reply
 *   'D' scheduler demo for length ms: two equal-priority CPU hogs plus a
 *       higher-priority 10 ms ticker, text reply
 *   'I' type length payload bytes into tty0 (keyboard injection)
 *   'V' tty0 screen text
 *   'M' SPI flash: send 64 KiB block number length (read only)
 *   'O' stack overflow test (the kernel panics and reboots)
 *   'F' HardFault (fault capture test)   'X' reboot
 * Pattern byte i = ((i * 0x9e3779b1) >> 24) ^ i. */
void diag_poll(void);                 /* kernel daemon: start a worker per connection */
void diag_ch376_bench(uint32_t length);

#endif
