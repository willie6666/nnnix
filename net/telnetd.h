/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NNNIX_TELNETD_H
#define NNNIX_TELNETD_H

void telnetd_init(void);              /* set up the port 23 listeners */
void telnetd_poll(void);              /* accept and reap sessions (kernel daemon) */

#endif
