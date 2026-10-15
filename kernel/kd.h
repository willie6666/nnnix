/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NNNIX_KD_H
#define NNNIX_KD_H

/* The kernel daemon: one task that runs every system service as an event loop
 * instead of a task (and a stack) per service. It wakes on W5500 and UART1
 * interrupts and on timers:
 *   events   W5500 socket events, keyboard input and key repeat
 *   50 ms    telnet and diag listeners, session reaping, console shell restart
 *   1 s      module supervision, LED; 60 s statistics
 * Work done here must not block for long; anything that does (shells, the diag
 * session) runs in its own task. */
void kd_init(void);
void kd_notify(void);                 /* wake the daemon; safe from interrupts */

#endif
