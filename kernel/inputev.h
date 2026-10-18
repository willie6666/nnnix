/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NNNIX_INPUTEV_H
#define NNNIX_INPUTEV_H
#include <stdint.h>
#include "../lib/sys.h"

/* /dev/input's events (host tested; kernel/input.c feeds them): a queue that
 * merges mouse motion, and the events between two keyboard reports or two
 * keypad states. */
#define INPUT_EVENTS 32

struct input_queue {
    struct nn_input_event ev[INPUT_EVENTS];
    uint8_t head, len;
    uint16_t lost;                   /* events dropped because the queue was full */
};

/* Queue e. Motion with the same buttons and no wheel joins the newest event
 * when that is such motion too and the sum fits. 0, or -1 (full: dropped). */
int inputq_put(struct input_queue *q, const struct nn_input_event *e);
/* The oldest event into *e: 1, or 0 when there is none. */
int inputq_get(struct input_queue *q, struct nn_input_event *e);

/* Key events from boot keyboard report `prev` to `now` (byte 0 modifiers,
 * bytes 2-7 keys): releases first, then presses, each with ms. caps: Caps
 * Lock is on (for the characters). The number of events written (at most
 * max); a report with ErrorRollOver (a usage of 1) gives none. */
int input_key_events(const uint8_t prev[8], const uint8_t now[8], int caps, uint16_t ms,
                     struct nn_input_event *out, int max);

/* The keypad: bit k-1 for key k (1-9). Events from state prev to now. */
int input_pad_events(uint16_t prev, uint16_t now, uint16_t ms, struct nn_input_event *out, int max);

#endif
