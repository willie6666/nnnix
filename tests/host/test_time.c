/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _DEFAULT_SOURCE
#include "../../lib/time.h"
#include "check.h"
#include <string.h>
#include <time.h>

int main(void)
{
    /* Against the host C library over 1970-2105, one sample every ~3.7 days plus edges. */
    for (uint64_t t = 0; t <= 0xffffffffull; t += 321247ull) {
        time_t ht = (time_t)t;
        struct tm h;
        gmtime_r(&ht, &h);
        struct nn_tm n;
        nn_gmtime((uint32_t)t, &n);
        CHECK_EQ_INT(n.year, h.tm_year + 1900);
        CHECK_EQ_INT(n.mon, h.tm_mon + 1);
        CHECK_EQ_INT(n.mday, h.tm_mday);
        CHECK_EQ_INT(n.hour * 3600 + n.min * 60 + n.sec, h.tm_hour * 3600 + h.tm_min * 60 + h.tm_sec);
        CHECK_EQ_INT(n.wday, h.tm_wday);
        CHECK_EQ_INT(nn_mktime(&n), t);
    }
    char buf[20];
    nn_format_time(0, buf);
    CHECK(!strcmp(buf, "1970-01-01 00:00:00"));
    nn_format_time(1792154096u, buf);                  /* 2026-10-16 12:34:56 */
    CHECK(!strcmp(buf, "2026-10-16 12:34:56"));
    uint32_t t = 0;
    CHECK(!nn_parse_time("2026-10-16 12:34:56", &t)); CHECK_EQ_INT(t, 1792154096u);
    CHECK(!nn_parse_time("2026-10-16T12:34", &t)); CHECK_EQ_INT(t, 1792154040u);
    CHECK(!nn_parse_time("2024-02-29 00:00", &t));
    CHECK(!nn_parse_time("2000-02-29 00:00", &t));
    const char *bad[] = { "2023-02-29 00:00", "2100-02-29 00:00", "2026-13-01 00:00", "2026-00-10 00:00",
        "2026-04-31 00:00", "2026-10-16 24:00", "2026-10-16 12:60", "2026-10-16 12:00:60", "1969-12-31 23:59",
        "2026-10-16", "2026-10-16 12", "2026-10-16 12:00x", "26-10-16 12:00", "2026/10/16 12:00", "" };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; ++i) CHECK(nn_parse_time(bad[i], &t) != 0);
    return check_summary("test_time");
}
