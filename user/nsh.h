/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NNNIX_NSH_H
#define NNNIX_NSH_H

/* Stack of a shell task: measured peak 952 B (2026-10-19, script arguments)
 * plus the 128-byte red zone, plus room. */
#define NSH_STACK 1152u

/* The nnnix shell. Reads lines from fd 0 and runs built-in commands until
 * "exit" or end of file. Uses only system calls. */
void nsh_main(void);
void nsh_boot(void *arg);            /* optional /etc/rc, before interactive shells */
int nsh_boot_done(void);

#endif
