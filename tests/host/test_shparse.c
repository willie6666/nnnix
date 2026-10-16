/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "check.h"
#include "../../user/shparse.h"
#include <string.h>

static struct sh_command c;
static const char *value(void *ctx, const char *name)
{
    (void)ctx;
    if (!strcmp(name, "X")) return "two words > literal";
    if (!strcmp(name, "?")) return "7";
    return "";
}

static const char *parse(const char *s) { return sh_parse(s, &c, value, 0); }

int main(void)
{
    CHECK(!parse(" # comment")); CHECK_EQ_INT(c.argc, 0);
    CHECK(!parse("echo one\"two\" 'three four' \"\" five\\ six # stop"));
    CHECK_EQ_INT(c.argc, 5);
    CHECK(!strcmp(c.argv[1], "onetwo")); CHECK(!strcmp(c.argv[2], "three four"));
    CHECK(!strcmp(c.argv[3], "")); CHECK(!strcmp(c.argv[4], "five six"));
    CHECK(c.argv[c.argc] == 0);
    CHECK(!parse("cat<in>out >>last")); CHECK_EQ_INT(c.argc, 1); CHECK_EQ_INT(c.nredir, 3);
    CHECK(!strcmp(c.redir[0].path, "in")); CHECK_EQ_INT(c.redir[0].target, 0);
    CHECK(!strcmp(c.redir[1].path, "out")); CHECK_EQ_INT(c.redir[2].append, 1);
    CHECK(!parse("echo '$X' \"${X}\" $X \\$X $? \"<literal>\""));
    CHECK_EQ_INT(c.argc, 7); CHECK(!strcmp(c.argv[1], "$X"));
    CHECK(!strcmp(c.argv[2], "two words > literal")); CHECK(!strcmp(c.argv[3], c.argv[2]));
    CHECK(!strcmp(c.argv[4], "$X")); CHECK(!strcmp(c.argv[5], "7"));
    CHECK(!strcmp(c.argv[6], "<literal>")); CHECK_EQ_INT(c.nredir, 0);
    CHECK(!parse("echo x#y $UNSET")); CHECK(!strcmp(c.argv[1], "x#y"));
    CHECK(!strcmp(c.argv[2], ""));
    const char *bad[] = { "cat >", "cat > # no path", "cat > <in", "cat >\"\"", "echo 'bad",
        "echo \"bad", "echo \\", "echo ${}", "echo ${X", "cat <<in", "cat >>>out", "a|b", "a&", "a;b",
        "a b c d e f g h i j k l", "cat >a >b >c >d >e" };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; ++i) CHECK(parse(bad[i]) != 0);
    char large[256]; memset(large, 'a', sizeof large); large[sizeof large - 1] = 0;
    CHECK(parse(large) != 0);
    CHECK(!parse("echo ok")); CHECK_EQ_INT(c.argc, 2); CHECK(c.argv[2] == 0);

    /* Pipelines and background jobs. */
    static struct sh_pipeline pl;
    struct sh_stage *p = pl.cmd;
    int n, bg;
#define PL(line) (sh_parse_line((line), &pl, value, 0) ? 1 : (n = pl.count, bg = pl.background, 0))
    CHECK(!PL("ls / | grep usb | wc > out"));
    CHECK_EQ_INT(n, 3); CHECK_EQ_INT(bg, 0);
    CHECK(!strcmp(p[0].argv[0], "ls")); CHECK(!strcmp(p[1].argv[1], "usb"));
    CHECK(!strcmp(p[2].argv[0], "wc")); CHECK_EQ_INT(p[2].nredir, 1);
    CHECK(!PL("sleep 5 &")); CHECK_EQ_INT(n, 1); CHECK_EQ_INT(bg, 1);
    CHECK(!PL("a|b&  # bg")); CHECK_EQ_INT(n, 2); CHECK_EQ_INT(bg, 1);
    CHECK(!PL("echo 'a|b' \\& \"x;y\""));
    CHECK_EQ_INT(n, 1); CHECK(!strcmp(p[0].argv[1], "a|b")); CHECK(!strcmp(p[0].argv[2], "&"));
    CHECK(!PL("   ")); CHECK_EQ_INT(n, 0);
    const char *badline[] = { "| a", "a |", "a | | b", "a || b", "&", "a & b", "a; b", "a|b|c|d", "a | > x" };
    for (unsigned i = 0; i < sizeof badline / sizeof badline[0]; ++i)
        CHECK(PL(badline[i]) != 0);
    return check_summary("test_shparse");
}
