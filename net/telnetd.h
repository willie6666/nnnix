/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NNNIX_TELNETD_H
#define NNNIX_TELNETD_H

void telnetd_init(void);              /* set up the port 23 listeners */
void telnetd_poll(void);              /* accept and reap sessions (kernel daemon) */
int  telnetd_max_sessions(int n);     /* set (0-3) when n >= 0; returns the limit, -1 if n is invalid */

#endif
