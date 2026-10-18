/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NNNIX_KBD_H
#define NNNIX_KBD_H
#include <stdint.h>

/* USB boot-keyboard reports to terminal bytes (host tested): ASCII with Shift
 * and Caps Lock, Ctrl+letter as control characters, Enter as CR, Backspace as
 * DEL, arrows/Home/End/Delete as ANSI sequences. A held key repeats after
 * 500 ms every 50 ms.
 * Keys for the console itself come out as bytes from 0x80 up, which no key
 * gives otherwise: the console acts on them and passes nothing on. */
#define KBD_PAGE_UP     0x80u        /* PageUp (with Shift too): scroll back a screen */
#define KBD_PAGE_DOWN   0x81u        /* PageDown */
#define KBD_LINE_UP     0x82u        /* Shift+Up: a line */
#define KBD_LINE_DOWN   0x83u        /* Shift+Down */
#define KBD_TEXT_SMALL  0x84u        /* Ctrl+-: small text, more of it */
#define KBD_TEXT_LARGE  0x85u        /* Ctrl+= (Ctrl++) or Ctrl+0: the normal size */
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
