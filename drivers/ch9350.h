#ifndef NNNIX_CH9350_H
#define NNNIX_CH9350_H
#include <stdint.h>

/* CH9350 lower-computer state 2 frames (DIP 1,1,1,1,0):
 *   57 AB 01 + 8-byte boot keyboard report
 *   57 AB 02 + 4-byte relative mouse report (buttons, dx, dy, wheel)
 *   57 AB 80 + status byte
 *   57 AB 86            device removed
 * The parser is pure so it can be unit tested on the host. */
enum { CH9350_NONE, CH9350_KEYBOARD, CH9350_MOUSE, CH9350_STATUS, CH9350_REMOVED };

struct ch9350_event { uint8_t type; uint8_t data[8]; };

struct ch9350_parser {
    uint8_t frame[11];
    uint8_t len, need;
    uint32_t bad_bytes;
};

/* Feed one byte; returns 1 and fills *ev when a frame completes. */
int ch9350_feed(struct ch9350_parser *p, uint8_t byte, struct ch9350_event *ev);

#endif
