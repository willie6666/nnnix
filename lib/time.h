/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NNNIX_TIME_H
#define NNNIX_TIME_H
#include <stdint.h>

/* Calendar time in UTC, as seconds since 1970-01-01 (unsigned: good until 2106). */
struct nn_tm {
    uint16_t year;                   /* 1970- */
    uint8_t mon, mday;               /* 1-12, 1-31 */
    uint8_t hour, min, sec;
    uint8_t wday;                    /* 0 = Sunday */
};

uint32_t nn_mktime(const struct nn_tm *t);         /* fields are not range checked */
void     nn_gmtime(uint32_t t, struct nn_tm *out);
/* "YYYY-MM-DD HH:MM[:SS]" or "YYYY-MM-DDTHH:MM[:SS]"; 0 on success. */
int      nn_parse_time(const char *s, uint32_t *out);
/* "2026-10-16 12:34:56" into buf (at least 20 bytes). */
void     nn_format_time(uint32_t t, char *buf);

#endif
