/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NNNIX_AUDIO_H
#define NNNIX_AUDIO_H
#include <stdint.h>

/* Sound through the onboard buzzer (PB11, low active): unsigned 8-bit mono
 * samples at AUDIO_RATE, 128 is silence. The buzzer is a magnetic one made for
 * a 2.4 kHz beep, so expect telephone-like sound, loudest around 2-3 kHz.
 *
 * PWM4 (group B channel 0) on PB11 runs at 98 kHz with 8-bit resolution; the
 * TIMER0 interrupt moves one sample from a ring buffer into its comparator
 * AUDIO_RATE times a second. The ring (AUDIO_RING bytes) exists only while
 * the device is open. */
#define AUDIO_RATE 8000u
#define AUDIO_RING 1024u             /* 128 ms */

int  audio_open(void);               /* 0, SYS_EBUSY or SYS_ENOMEM */
/* Queue samples; blocks while the ring is full, and while another task
 * sharing the open file writes. Returns the number queued: fewer than n only
 * when the calling task is killed. */
uint32_t audio_write(const uint8_t *p, uint32_t n);
/* Stop: drain what is queued first (at most AUDIO_RING samples) unless the
 * caller was killed, then silence the buzzer and free the ring. Logs the
 * samples played and the underruns (samples missing between the first write
 * and the close). */
void audio_close(void);

#endif
