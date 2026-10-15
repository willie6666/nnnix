#include "../../lib/fmt.h"
#include "../../lib/string.h"
#include "check.h"

static int fmt_is(const char *expected, const char *fmt, ...)
{
    char buf[64];
    va_list ap;
    va_start(ap, fmt);
    int n = nn_vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (strcmp(buf, expected) || n != (int)strlen(expected)) {
        fprintf(stderr, "format \"%s\": got \"%s\" (%d), expected \"%s\"\n", fmt, buf, n, expected);
        return 0;
    }
    return 1;
}

int main(void)
{
    CHECK(fmt_is("42", "%d", 42));
    CHECK(fmt_is("-7", "%d", -7));
    CHECK(fmt_is("-2147483648", "%d", (int)0x80000000));
    CHECK(fmt_is("4294967295", "%u", 0xffffffffu));
    CHECK(fmt_is("00ff", "%04x", 255u));
    CHECK(fmt_is("ABCD", "%X", 0xabcdu));
    CHECK(fmt_is("[   12.345] ", "[%5u.%03u] ", 12u, 345u));
    CHECK(fmt_is("ab   |", "%-5s|", "ab"));
    CHECK(fmt_is("   ab|", "%5s|", "ab"));
    CHECK(fmt_is("  -5", "%4d", -5));
    CHECK(fmt_is("-005", "%04d", -5));
    CHECK(fmt_is("x|", "%c|", 'x'));
    CHECK(fmt_is("100%", "%d%%", 100));
    CHECK(fmt_is("(null)", "%s", (char *)0));
    CHECK(fmt_is("0x0000beef", "%p", (void *)0xbeef));
    CHECK(fmt_is("   7", "%*d", 4, 7));

    char small[5];
    CHECK_EQ_INT(nn_snprintf(small, sizeof small, "%s", "abcdefgh"), 8);
    CHECK(strcmp(small, "abcd") == 0);

    char overlap[] = "0123456789";
    memmove(overlap + 2, overlap, 5);
    CHECK(strcmp(overlap, "0101234789") == 0);
    memmove(overlap, overlap + 2, 5);
    CHECK(strcmp(overlap, "0123434789") == 0);
    CHECK(memcmp("abc", "abd", 3) < 0);
    CHECK(strncmp("abcdef", "abcxyz", 3) == 0);
    return check_summary("test_lib");
}
