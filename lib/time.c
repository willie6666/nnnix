/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Civil date <-> day number after Howard Hinnant's public-domain algorithms. */
#include "time.h"

static uint32_t days_from_civil(uint32_t y, uint32_t m, uint32_t d)
{
    y -= m <= 2;
    uint32_t era = y / 400u, yoe = y - era * 400u;
    uint32_t doy = (153u * (m > 2 ? m - 3u : m + 9u) + 2u) / 5u + d - 1u;
    uint32_t doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097u + doe - 719468u;
}

uint32_t nn_mktime(const struct nn_tm *t)
{
    uint32_t days = days_from_civil(t->year, t->mon, t->mday);
    return days * 86400u + t->hour * 3600u + t->min * 60u + t->sec;
}

void nn_gmtime(uint32_t t, struct nn_tm *out)
{
    uint32_t days = t / 86400u, rest = t - days * 86400u;
    out->hour = (uint8_t)(rest / 3600u);
    rest -= out->hour * 3600u;
    out->min = (uint8_t)(rest / 60u);
    out->sec = (uint8_t)(rest - out->min * 60u);
    out->wday = (uint8_t)((days + 4u) % 7u);       /* 1970-01-01 was a Thursday */
    uint32_t z = days + 719468u, era = z / 146097u, doe = z - era * 146097u;
    uint32_t yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    uint32_t doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    uint32_t mp = (5u * doy + 2u) / 153u;
    out->mday = (uint8_t)(doy - (153u * mp + 2u) / 5u + 1u);
    out->mon = (uint8_t)(mp < 10u ? mp + 3u : mp - 9u);
    out->year = (uint16_t)(yoe + era * 400u + (out->mon <= 2u));
}

static const char *number(const char *s, unsigned digits, unsigned *out)
{
    unsigned v = 0;
    if (!s) return 0;
    for (unsigned i = 0; i < digits; ++i, ++s) {
        if (*s < '0' || *s > '9') return 0;
        v = v * 10u + (unsigned)(*s - '0');
    }
    *out = v;
    return s;
}

static const char *expect(const char *s, char c) { return s && *s == c ? s + 1 : 0; }

int nn_parse_time(const char *s, uint32_t *out)
{
    unsigned y, mo, d, h, mi, sec = 0;
    s = number(s, 4, &y);
    s = s ? number(expect(s, '-'), 2, &mo) : 0;
    s = s ? number(expect(s, '-'), 2, &d) : 0;
    if (!s || (*s != ' ' && *s != 'T')) return -1;
    s = number(s + 1, 2, &h);
    s = s ? number(expect(s, ':'), 2, &mi) : 0;
    if (s && *s == ':') s = number(s + 1, 2, &sec);
    if (!s || *s) return -1;
    static const uint8_t mdays[12] = { 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (y < 1970 || y > 2099 || mo < 1 || mo > 12 || d < 1 || d > mdays[mo - 1] || h > 23 || mi > 59 || sec > 59)
        return -1;
    if (mo == 2 && d == 29 && (y % 4u || (y % 100u == 0 && y % 400u))) return -1;
    struct nn_tm t = { (uint16_t)y, (uint8_t)mo, (uint8_t)d, (uint8_t)h, (uint8_t)mi, (uint8_t)sec, 0 };
    *out = nn_mktime(&t);
    return 0;
}

static char *two(char *p, unsigned v) { p[0] = (char)('0' + v / 10u); p[1] = (char)('0' + v % 10u); return p + 2; }

void nn_format_time(uint32_t t, char *buf)
{
    struct nn_tm tm;
    nn_gmtime(t, &tm);
    char *p = two(two(buf, tm.year / 100u), tm.year % 100u);
    *p++ = '-'; p = two(p, tm.mon);
    *p++ = '-'; p = two(p, tm.mday);
    *p++ = ' '; p = two(p, tm.hour);
    *p++ = ':'; p = two(p, tm.min);
    *p++ = ':'; p = two(p, tm.sec);
    *p = 0;
}
