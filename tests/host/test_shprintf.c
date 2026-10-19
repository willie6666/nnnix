/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdint.h>
#include <string.h>
#include "../../user/shprintf.h"
#include "../../lib/sys.h"
#include "check.h"

static unsigned char output[4096];
static unsigned written, errors;
static int mode, capture;
enum { OK, SHORT, ZERO, FAIL, EARLY_CANCEL, LATE_CANCEL };

intptr_t host_syscall(int n, intptr_t a, intptr_t b, intptr_t c)
{
    if (n != SYS_WRITE) { CHECK(0); return SYS_ENOSYS; }
    if (!capture) return (intptr_t)fwrite((const void *)b, 1, (size_t)c, a == 2 ? stderr : stdout);
    if (a == 2) { errors += (unsigned)c; return c; }
    CHECK_EQ_INT(a, 1);
    if (mode == ZERO) return 0;
    if (mode == FAIL) return SYS_EIO;
    if (mode == SHORT && c > 3) c = 3;
    CHECK(written + (unsigned)c <= sizeof output);
    memcpy(output + written, (const void *)b, (size_t)c);
    written += (unsigned)c;
    return c;
}
int nn_interrupted(void) { return mode == EARLY_CANCEL || (mode == LATE_CANCEL && written >= 32); }

static int run(int test, int argc, char **argv)
{
    mode = test; capture = 1; written = errors = 0;
    memset(output, 0xa5, sizeof output);
    return cmd_printf(argc, argv);
}

static void good(const char *format, const char *arg, const unsigned char *expected, unsigned len)
{
    char *argv[] = { "printf", (char *)format, (char *)arg };
    CHECK_EQ_INT(run(OK, arg ? 3 : 2, argv), 0);
    CHECK_EQ_INT(written, len);
    CHECK(!memcmp(output, expected, len));
    CHECK_EQ_INT(errors, 0);
}

int main(void)
{
    good("\\0\\x00\\xff\\0377\\101", 0, (const unsigned char *)"\0\0\xff\xff" "A", 5);
    good("%b", "\\x00\\xff\\n\\r\\t\\\\", (const unsigned char *)"\0\xff\n\r\t\\", 6);
    good("%s", "\\x00", (const unsigned char *)"\\x00", 4);
    good("%%:%s:%c", "hello", (const unsigned char *)"%:hello:\0", 9);
    good("%d", "-2147483648", (const unsigned char *)"-2147483648", 11);
    good("%u", "4294967295", (const unsigned char *)"4294967295", 10);
    good("%x", "0xFFFFFFFF", (const unsigned char *)"ffffffff", 8);
    good("%X", "255", (const unsigned char *)"FF", 2);
    good("%B", "0x80", (const unsigned char *)"\x80", 1);
    good("constant", "unused", (const unsigned char *)"constant", 8);
    good("%s", 0, (const unsigned char *)"", 0);
    good("%B", 0, (const unsigned char *)"\0", 1);
    good("", "unused", (const unsigned char *)"", 0);

    /* Every byte value, including NUL and signed-char boundaries. */
    for (unsigned byte = 0; byte < 256; ++byte) {
        char text[4]; snprintf(text, sizeof text, "%u", byte);
        unsigned char want = (unsigned char)byte;
        good("%B", text, &want, 1);
    }
    char *repeat[] = { "printf", "<%s>", "one", "two", "" };
    CHECK_EQ_INT(run(OK, 5, repeat), 0);
    CHECK_EQ_INT(written, 12);
    CHECK(!memcmp(output, "<one><two><>", 12));
    char *bad[][3] = {
        { "printf", 0, 0 }, { "printf", "%", 0 }, { "printf", "%q", 0 },
        { "printf", "\\", 0 }, { "printf", "\\x", 0 }, { "printf", "\\q", 0 },
        { "printf", "\\400", 0 }, { "printf", "%b", "okay\\0777" },
        { "printf", "%B", "256" }, { "printf", "%B", "-1" },
        { "printf", "%B", "0x100" }, { "printf", "%u", "4294967296" },
        { "printf", "%d", "2147483648" }, { "printf", "%d", "-2147483649" },
        { "printf", "%B", "1x" }, { "printf", "%B", "" }, { "printf", "%02x", "3" }
    };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        CHECK_EQ_INT(run(OK, !bad[i][1] ? 1 : bad[i][2] ? 3 : 2, bad[i]), 2);
        CHECK_EQ_INT(written, 0); CHECK(errors > 0);
    }
    char *later_bad[] = { "printf", "%B", "1", "255", "256" };
    CHECK_EQ_INT(run(OK, 5, later_bad), 2);
    CHECK_EQ_INT(written, 0);          /* validate subsequent format repetitions too */
    char long_text[100]; memset(long_text, 'a', sizeof long_text); long_text[99] = 0;
    char *stream[] = { "printf", "%s", long_text };
    CHECK_EQ_INT(run(SHORT, 3, stream), 0);
    CHECK_EQ_INT(written, 99); CHECK(!memcmp(output, long_text, 99));
    CHECK_EQ_INT(run(ZERO, 3, stream), 1); CHECK(errors > 0);
    CHECK_EQ_INT(run(FAIL, 3, stream), 1); CHECK(errors > 0);
    CHECK_EQ_INT(run(EARLY_CANCEL, 3, stream), 130); CHECK_EQ_INT(written, 0);
    CHECK_EQ_INT(run(LATE_CANCEL, 3, stream), 130); CHECK_EQ_INT(written, 32);
    capture = 0;
    return check_summary("test_shprintf");
}
