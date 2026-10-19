/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NNNIX_INPUTEV_H
#define NNNIX_INPUTEV_H
#include <stdint.h>
#include "../lib/sys.h"

/* /dev/input's events (host tested; kernel/input.c feeds them): a queue that
 * merges mouse motion, and the events between two keyboard reports or two
 * keypad states. */
#define INPUT_EVENTS 32
#define INPUT_KEY_EVENTS 20          /* between two keyboard reports: 8 modifiers, 6 keys up, 6 down */

/* What the reader has been told so far: the keys, mouse buttons and keypad
 * keys down as of the events queued. */
struct input_told {
    uint8_t keys[32];                /* a bit per HID usage */
    uint8_t buttons;
    uint16_t pad;                    /* bit k-1 for key k */
};

struct input_queue {
    struct nn_input_event ev[INPUT_EVENTS];
    uint8_t head, len;
    uint8_t resync;                  /* events were dropped: queue the difference first */
    uint16_t lost;                   /* events dropped because the queue was full */
    struct input_told told;
};

/* The state the input really is in, for inputq_sync. */
struct input_state {
    const uint8_t *report;           /* the last valid boot keyboard report */
    int caps;
    uint8_t buttons;
    uint16_t pad;
};

/* Queue e. Motion with the same buttons and no wheel joins the newest event
 * when that is such motion too and the sum fits. 0, or -1 when it was
 * dropped: the queue was full, or still is behind since it was (resync). */
int inputq_put(struct input_queue *q, const struct nn_input_event *e);
/* The oldest event into *e: 1, or 0 when there is none. */
int inputq_get(struct input_queue *q, struct nn_input_event *e);
/* After a drop (resync): queue the events that take the reader from what it
 * was told to the state `now` (releases first), as far as there is room, so
 * no release stays lost (review R51). Then events queue again. */
void inputq_sync(struct input_queue *q, const struct input_state *now, uint16_t ms);

/* Key events from boot keyboard report `prev` to `now` (byte 0 modifiers,
 * bytes 2-7 keys): releases first, then presses, each with ms. caps: Caps
 * Lock is on (for the characters). The number of events written (at most
 * max); a report with ErrorRollOver (a usage of 1) gives none. */
int input_key_events(const uint8_t prev[8], const uint8_t now[8], int caps, uint16_t ms,
                     struct nn_input_event *out, int max);
/* A report to take: not ErrorRollOver (a usage of 1, "too many keys"), which
 * says nothing about the keys and must not become the last report (R52). */
int input_report_valid(const uint8_t r[8]);

/* The keypad: bit k-1 for key k (1-9). Events from state prev to now. */
int input_pad_events(uint16_t prev, uint16_t now, uint16_t ms, struct nn_input_event *out, int max);

#endif
