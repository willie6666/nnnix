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
 *   'O' stack overflow test (the kernel panics and reboots)
 *   'F' HardFault (fault capture test)   'X' reboot
 * Pattern byte i = ((i * 0x9e3779b1) >> 24) ^ i. Replaced by telnetd in M2. */
void diag_task(void *arg);
void diag_ch376_bench(uint32_t length);   /* also run by the F1 key */

#endif
