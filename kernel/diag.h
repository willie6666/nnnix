/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NNNIX_DIAG_H
#define NNNIX_DIAG_H

/* Diagnostic TCP service on W5500 socket 0, port 5000 (used by tools/nntest.py).
 * Request: cmd (1 byte), SPI divider (1 byte), length (u32 big endian).
 *   'P' banner text          'S' status text            'L' kernel log ring
 *   'T' send length pattern bytes
 *   'R' receive length pattern bytes, reply u32 BE: length, errors, ms
 *   'U' CH376S write/read/verify/delete of length bytes, text reply
 *   'F' HardFault (fault capture test)   'X' reboot
 * Pattern byte i = ((i * 0x9e3779b1) >> 24) ^ i. Replaced by telnetd in M2. */
void diag_start(void);
void diag_poll(void);
void diag_ch376_bench(unsigned long length);   /* also used by the F1 key */

#endif
