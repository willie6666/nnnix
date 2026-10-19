/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NNNIX_MODULES_H
#define NNNIX_MODULES_H
#include <stdint.h>

/* External module supervision (MB-102 rail, CH376S, W5500, Ethernet link). */
/* ch376: 0 while the CH376S does not answer; else a number (1-63) that is new
 * each time it comes back (it lost its state: power, or it stopped
 * answering), so usbfs mounts again and fails the files opened before.
 * Written by the kernel daemon only. */
struct modules_state {
    uint8_t powered : 1, w5500 : 1, last_ch376 : 6;   /* last_ch376: the last nonzero ch376 */
    uint8_t ch376, link;
};
extern struct modules_state modules;

void modules_poll(void);              /* call about once per second */

#endif
