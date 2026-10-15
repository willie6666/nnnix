/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NNNIX_NSH_H
#define NNNIX_NSH_H

/* The nnnix shell. Reads lines from fd 0 and runs built-in commands until
 * "exit" or end of file. Uses only system calls. */
void nsh_main(void);

#endif
