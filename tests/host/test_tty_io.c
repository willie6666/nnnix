/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <string.h>
#include "../../kernel/tty.h"
#include "check.h"

uint32_t jiffies;
void mutex_lock(struct mutex *m) { (void)m; }
void mutex_unlock(struct mutex *m) { (void)m; }

static struct tty tty;
static const unsigned char *input;
static char output[256];
static unsigned calls, interrupt_at, interrupt_byte, poll_hangup, output_len;

static int get(void *ctx, uint32_t timeout)
{
    (void)ctx;
    ++calls;
    if (calls == interrupt_at) {
        tty_interrupt(&tty); jiffies += timeout;
        return interrupt_byte && *input ? *input++ : TTY_TIMEOUT;
    }
    return *input ? *input++ : -1;
}

static int poll_byte(void *ctx) { (void)ctx; return *input ? *input++ : poll_hangup ? TTY_HANGUP : -1; }
static void write_bytes(void *ctx, const char *s, size_t n)
{
    (void)ctx;
    if (n > sizeof output - output_len - 1) n = sizeof output - output_len - 1;
    memcpy(output + output_len, s, n); output_len += (unsigned)n; output[output_len] = 0;
}
static const struct tty_ops ops = { get, write_bytes, poll_byte };

static void setup(const char *text)
{
    memset(&tty, 0, sizeof tty);
    tty_init(&tty, &ops, 0);
    input = (const unsigned char *)text;
    calls = interrupt_at = interrupt_byte = poll_hangup = output_len = jiffies = 0;
    output[0] = 0;
}

int main(void)
{
    char buf[32];
    setup("hi\n");
    CHECK_EQ_INT(tty_read(&tty, buf, sizeof buf), 3);
    CHECK(!memcmp(buf, "hi\n", 3));
    CHECK_EQ_INT(tty.reading, 0);

    setup("junk\x03" "ok\n");
    CHECK_EQ_INT(tty_read(&tty, buf, sizeof buf), -EINTR);
    CHECK_EQ_INT(tty.ed.len, 0);
    CHECK_EQ_INT(tty.reading, 0);
    CHECK(strstr(output, "^C") != 0);
    CHECK_EQ_INT(tty_read(&tty, buf, sizeof buf), 3);
    CHECK(!memcmp(buf, "ok\n", 3));

    setup("unfinished"); poll_hangup = 1;
    CHECK_EQ_INT(tty_interrupted(&tty), 1);
    CHECK_EQ_INT(tty.ahead_len, 0);
    CHECK_EQ_INT(tty_read(&tty, buf, sizeof buf), 0);

    /* An out-of-band cancel bypasses a full type-ahead queue and also drops
     * unread line fragments. It is consumed once and preserves the terminal. */
    setup("ok\n");
    memset(tty.ahead, 'x', sizeof tty.ahead); tty.ahead_len = sizeof tty.ahead;
    memcpy(tty.line, "old\n", 4); tty.rpos = 1; tty.rlen = 4;
    tty_interrupt(&tty);
    CHECK_EQ_INT(tty_read(&tty, buf, sizeof buf), -EINTR);
    CHECK_EQ_INT(tty.ahead_len, 0);
    CHECK_EQ_INT(tty.rlen, 0);
    CHECK_EQ_INT(tty.interrupt_pending, 0);
    CHECK_EQ_INT(tty_read(&tty, buf, sizeof buf), 3);
    CHECK(!memcmp(buf, "ok\n", 3));

    /* Simulate SW_INT while the backend waits after an unfinished escape. */
    setup("ab\x1b["); interrupt_at = 5;
    CHECK_EQ_INT(tty_read(&tty, buf, sizeof buf), -EINTR);
    CHECK_EQ_INT(tty.ed.len, 0);
    CHECK_EQ_INT(tty.ed.esc, 0);
    CHECK_EQ_INT(tty.reading, 0);
    input = (const unsigned char *)"ok\n";
    CHECK_EQ_INT(tty_read(&tty, buf, sizeof buf), 3);
    CHECK(!memcmp(buf, "ok\n", 3));

    setup("");
    memset(tty.ahead, 'x', sizeof tty.ahead); tty.ahead_len = sizeof tty.ahead;
    tty_interrupt(&tty);
    tty.reading = 1;
    CHECK_EQ_INT(tty_interrupted(&tty), 0);    /* no foreground check steals a reader's cancel */
    CHECK_EQ_INT(tty.interrupt_pending, 1);
    tty.reading = 0;
    CHECK_EQ_INT(tty_interrupted(&tty), 1);
    CHECK_EQ_INT(tty.ahead_len, 0);
    CHECK_EQ_INT(tty_interrupted(&tty), 0);

    setup("0123456789abcdefmore\x03" "ok\n");
    CHECK_EQ_INT(tty_interrupted(&tty), 1);
    CHECK_EQ_INT(tty.ahead_len, 0);
    CHECK_EQ_INT(tty_read(&tty, buf, sizeof buf), 3);
    CHECK(!memcmp(buf, "ok\n", 3));

    setup("0123456789abcde\n");  /* fits: no byte is lost */
    CHECK_EQ_INT(tty_interrupted(&tty), 0);
    CHECK_EQ_INT(tty_read(&tty, buf, sizeof buf), 16);
    CHECK(!memcmp(buf, "0123456789abcde\n", 16));

    setup("0123456789abcdefoverflow\nok\n");
    CHECK_EQ_INT(tty_interrupted(&tty), 0);
    CHECK_EQ_INT(tty_read(&tty, buf, sizeof buf), 3);  /* whole overlong line discarded */
    CHECK(!memcmp(buf, "ok\n", 3));

    setup("0123456789abcdefoverflow");
    CHECK_EQ_INT(tty_interrupted(&tty), 0);
    input = (const unsigned char *)"more\nok\n";
    CHECK_EQ_INT(tty_read(&tty, buf, sizeof buf), 3);
    CHECK(!memcmp(buf, "ok\n", 3));

    char flood[261];
    memset(flood, 'x', 256); memcpy(flood + 256, "\x03" "ok\n", 4);
    flood[260] = 0;
    setup(flood);
    CHECK_EQ_INT(tty_interrupted(&tty), 0);
    CHECK_EQ_INT(tty_interrupted(&tty), 0);
    CHECK_EQ_INT(tty_interrupted(&tty), 1);  /* bounded checks eventually reach the cancel */

    setup("echo\n"); interrupt_at = interrupt_byte = 1;
    CHECK_EQ_INT(tty_read(&tty, buf, sizeof buf), -EINTR);
    CHECK_EQ_INT(tty_read(&tty, buf, sizeof buf), 5);
    CHECK(!memcmp(buf, "echo\n", 5));

    setup("ab\x1b[\x03" "ok\n");
    CHECK_EQ_INT(tty_read(&tty, buf, sizeof buf), -EINTR);
    CHECK_EQ_INT(tty.ed.esc, 0);
    CHECK_EQ_INT(tty_read(&tty, buf, sizeof buf), 3);
    CHECK(!memcmp(buf, "ok\n", 3));

    /* A pending hardware cancel works even without a polling backend. */
    const struct tty_ops no_poll = { get, write_bytes, 0 };
    tty_init(&tty, &no_poll, 0);
    tty_interrupt(&tty);
    CHECK_EQ_INT(tty_interrupted(&tty), 1);
    CHECK_EQ_INT(tty_interrupted(&tty), 0);
    return check_summary("test_tty_io");
}
