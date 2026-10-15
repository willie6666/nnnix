/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <string.h>
#include "../../kernel/ledit.h"
#include "../../net/telnet.h"
#include "check.h"

static char echoed[256];
static size_t echoed_len;

static void out(void *ctx, const char *s, size_t n)
{
    (void)ctx;
    for (size_t i = 0; i < n && echoed_len + 1 < sizeof echoed; ++i) echoed[echoed_len++] = s[i];
    echoed[echoed_len] = 0;
}

/* Feed a string literal; return the result of the last byte. */
#define FEED(e, lit) feed((e), (lit), sizeof(lit) - 1)
static int feed(struct ledit *e, const char *s, size_t n)
{
    int r = LEDIT_MORE;
    for (size_t i = 0; i < n; ++i) r = ledit_feed(e, (uint8_t)s[i]);
    return r;
}

static void test_ledit(void)
{
    char buf[16];
    struct ledit e;
    ledit_init(&e, buf, sizeof buf, out, 0);

    CHECK_EQ_INT(FEED(&e, "ls -l\r"), LEDIT_LINE);
    CHECK(strcmp(buf, "ls -l") == 0);
    CHECK(strcmp(echoed, "ls -l\r\n") == 0);

    /* CR LF and CR NUL count as one line end; the LF must not end an empty line. */
    e.len = 0; echoed_len = 0;
    CHECK_EQ_INT(ledit_feed(&e, '\n'), LEDIT_MORE);
    CHECK_EQ_INT(FEED(&e, "pwd\r"), LEDIT_LINE);
    CHECK_EQ_INT(ledit_feed(&e, 0), LEDIT_MORE);
    CHECK_EQ_INT(FEED(&e, "x\n"), LEDIT_LINE);       /* a new line starts by itself */
    CHECK(strcmp(buf, "x") == 0);

    /* Backspace and DEL erase; extra erasures are harmless. */
    e.len = 0; echoed_len = 0;
    CHECK_EQ_INT(FEED(&e, "ecxo\x7f\x7fho\b\bho hi\r"), LEDIT_LINE);
    CHECK(strcmp(buf, "echo hi") == 0);
    e.len = 0;
    CHECK_EQ_INT(FEED(&e, "\x7f\x7f" "a\r"), LEDIT_LINE);
    CHECK(strcmp(buf, "a") == 0);

    /* Ctrl-U kills the line, Ctrl-C interrupts, Ctrl-D only on an empty line. */
    e.len = 0;
    CHECK_EQ_INT(FEED(&e, "junk\x15ok\r"), LEDIT_LINE);
    CHECK(strcmp(buf, "ok") == 0);
    e.len = 0; echoed_len = 0;
    CHECK_EQ_INT(FEED(&e, "abc\x03"), LEDIT_INTR);
    CHECK_EQ_INT(e.len, 0);
    CHECK(strstr(echoed, "^C") != NULL);
    CHECK_EQ_INT(FEED(&e, "ab\x04"), LEDIT_MORE);
    e.len = 0;
    CHECK_EQ_INT(ledit_feed(&e, 0x04), LEDIT_EOF);

    /* Arrow keys (ESC [ A) and SS3 keys (ESC O P) are swallowed. */
    e.len = 0;
    CHECK_EQ_INT(FEED(&e, "a\x1b[A\x1b[1;5Cb\x1bOPc\r"), LEDIT_LINE);
    CHECK(strcmp(buf, "abc") == 0);

    /* The buffer keeps room for the NUL and rings the bell when full. */
    e.len = 0; echoed_len = 0;
    CHECK_EQ_INT(FEED(&e, "0123456789abcdefXYZ\r"), LEDIT_LINE);
    CHECK_EQ_INT(strlen(buf), sizeof buf - 1);
    CHECK(strchr(echoed, '\a') != NULL);
}

static void test_telnet(void)
{
    struct telnet t = { 0 };
    const uint8_t in[] = { 'a', TELNET_IAC, TELNET_DO, TELNET_OPT_ECHO, 'b',
                           TELNET_IAC, TELNET_SB, 31, 0, 80, 0, 24, TELNET_IAC, TELNET_SE, 'c',
                           TELNET_IAC, TELNET_IAC, TELNET_IAC, 241 /* NOP */, 'd' };
    uint8_t outb[sizeof in];
    int n = 0;
    for (size_t i = 0; i < sizeof in; ++i) {
        int r = telnet_rx(&t, in[i]);
        if (r >= 0) outb[n++] = (uint8_t)r;
    }
    CHECK_EQ_INT(n, 5);
    CHECK(memcmp(outb, "abc\xff" "d", 5) == 0);
    CHECK_EQ_INT(telnet_greeting[0], TELNET_IAC);
}

int main(void)
{
    test_ledit();
    test_telnet();
    return check_summary("test_tty");
}
