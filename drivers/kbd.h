/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NNNIX_KBD_H
#define NNNIX_KBD_H
#include <stdint.h>

/* USB boot-keyboard reports to terminal bytes (host tested): ASCII with Shift
 * and Caps Lock, Ctrl+letter as control characters, Enter as CR, Backspace as
 * DEL, arrows/Home/End/Delete as ANSI sequences. A held key repeats after
 * 500 ms every 50 ms. */
#define KBD_REPEAT_DELAY 500u
#define KBD_REPEAT_RATE  50u

struct kbd {
    uint8_t prev[8];
    uint8_t caps;
    uint8_t held, held_mods;         /* key that repeats, 0 if none */
    uint32_t repeat_at;
};

/* Bytes produced by keys newly pressed in this report (at most 4 per key). */
int kbd_report(struct kbd *k, const uint8_t report[8], uint32_t now, uint8_t *out, int max);
/* Bytes for auto-repeat when due; 0 otherwise. */
int kbd_repeat(struct kbd *k, uint32_t now, uint8_t *out, int max);

#endif
