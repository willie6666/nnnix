#ifndef NNNIX_MODULES_H
#define NNNIX_MODULES_H
#include <stdint.h>

/* External module supervision (MB-102 rail, CH376S, W5500, Ethernet link). */
struct modules_state { uint8_t powered, ch376, w5500, link; };
extern struct modules_state modules;

void modules_poll(void);              /* call about once per second */

#endif
