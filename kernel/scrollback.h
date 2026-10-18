/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NNNIX_SCROLLBACK_H
#define NNNIX_SCROLLBACK_H
#include <stdint.h>

/* The console's scrollback (host tested): rows that left the top of the
 * screen, oldest first, each trimmed of its trailing blanks and ended by
 * '\n', in a ring of bytes. When the ring is full the oldest bytes go; a line
 * that lost its beginning that way is no longer one of the lines. */
struct scrollback {
    char *buf;
    uint32_t size, start, end;       /* [start, end): what the ring holds, as byte counts */
    uint32_t newlines;
    uint8_t cut;                     /* the first line lost its beginning */
};

void scrollback_init(struct scrollback *s, char *buf, uint32_t size);   /* buf 0: none */
void scrollback_push(struct scrollback *s, const char *row, int n);
uint32_t scrollback_lines(const struct scrollback *s);
/* Line `back` from the newest (0) into row, padded with blanks to n: 1, or 0
 * if there is no such line. */
int scrollback_line(const struct scrollback *s, uint32_t back, char *row, int n);
/* Take the newest line out again (into row as above): 1, or 0 if none. */
int scrollback_pop(struct scrollback *s, char *row, int n);

#endif
