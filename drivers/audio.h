/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NNNIX_AUDIO_H
#define NNNIX_AUDIO_H
#include <stdint.h>

/* Sound through the onboard buzzer (PB11, low active): unsigned 8-bit mono
 * samples at AUDIO_RATE, 128 is silence. The buzzer is a magnetic one made for
 * a 2.4 kHz beep, so expect telephone-like sound, loudest around 2-3 kHz.
 *
 * PWM4 (group B channel 0) on PB11 runs at 98 kHz with 8-bit resolution; the
 * TIMER0 interrupt moves one sample from a ring buffer (drivers/audioring.h)
 * into its comparator AUDIO_RATE times a second. The ring (AUDIO_RING bytes,
 * or AUDIO_RING_MIN when memory is short; long enough to ride out a flash
 * erase in fsd that the player's reads wait behind) exists only while the
 * device is open. Opened for ADPCM (/dev/adpcm), it takes NNV sound records
 * (lib/nnv.h) and the interrupt decodes them: the ring then holds twice as
 * long, 512 ms (256 ms) less 3% for the records' headers. */
#define AUDIO_RATE 8000u
#define AUDIO_RING 2048u             /* 256 ms of samples */
#define AUDIO_RING_MIN 1024u         /* 128 ms */
#define AUDIO_WAKE_ADPCM 32u         /* a waiting ADPCM writer goes on once this much is free: 8 ms */

int  audio_open(int adpcm);          /* 0, SYS_EBUSY or SYS_ENOMEM */
/* Queue samples, or ADPCM sound records; blocks while the ring is full, and
 * while another task sharing the open file writes. Returns the number of bytes
 * taken: fewer than n only when the calling task is killed. SYS_EINVAL when
 * an ADPCM record is not a sound record: the stream starts over with the next
 * write, at a record header. */
int  audio_write(const uint8_t *p, uint32_t n);
/* Stop: drain what is queued first (at most one ring of samples) unless the
 * caller was killed, then silence the buzzer and free the ring. Logs the
 * samples played, the underruns (samples missing between the first write
 * and the close), the interrupt's latency and its longest run, and the
 * ring's size. */
void audio_close(void);

#endif
