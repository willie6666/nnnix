/* SPDX-License-Identifier: GPL-3.0-or-later */
/* bin/url.c: URLs parsed, redirects resolved against them (RFC 3986 5.2 and
 * its examples, for http), numbers read strictly. */
#include <stdint.h>
#include <string.h>
#include "../../bin/url.h"
#include "check.h"

static char url[192];
static struct url u;

static int parse(const char *s)
{
    strcpy(url, s);
    return url_parse(url, &u);
}

/* base resolved with ref, then parsed: the URL, or "" when either fails. */
static const char *resolved(const char *base, const char *ref)
{
    if (parse(base) || url_resolve(url, sizeof url, &u, ref) || url_parse(url, &u)) return "";
    return url;
}

#define RESOLVES(base, ref, want) do { \
    const char *got_ = resolved(base, ref); \
    check_count++; \
    if (strcmp(got_, want)) { check_failures++; fprintf(stderr, "%s:%d: %s + %s = \"%s\", expected \"%s\"\n", \
                                                        __FILE__, __LINE__, base, ref, got_, want); } \
} while (0)

int main(void)
{
    CHECK_EQ_INT(parse("http://example.org/a/b?x=1#top"), 0);
    CHECK(!strcmp(u.host, "example.org") && u.port == 80 && !strcmp(url + u.path, "/a/b?x=1"));
    CHECK_EQ_INT(parse("HTTP://h:8080"), 0);
    CHECK(!strcmp(u.host, "h") && u.port == 8080 && !strcmp(url + u.path, ""));
    CHECK_EQ_INT(parse("h?q"), 0);
    CHECK(!strcmp(url + u.path, "?q"));
    CHECK_EQ_INT(parse("h/a/./b/../../c/?d=../e"), 0);
    CHECK(!strcmp(url + u.path, "/c/?d=../e"));
    CHECK_EQ_INT(parse("h/../../x/.."), 0);
    CHECK(!strcmp(url + u.path, "/"));
    CHECK_EQ_INT(parse("https://h/"), -2);
    CHECK_EQ_INT(parse("ftp://h/"), -1);
    CHECK_EQ_INT(parse("http://h:0/"), -1);
    CHECK_EQ_INT(parse("http://h:65536/"), -1);
    CHECK_EQ_INT(parse("http://h:99999999999/"), -1);
    CHECK_EQ_INT(parse("http://h:80x/"), -1);
    CHECK_EQ_INT(parse("http://:80/"), -1);
    CHECK_EQ_INT(parse("http:///"), -1);

    /* RFC 3986 5.4.1, the base http://a/b/c/d;p?q (absolute and "g:h" aside). */
    const char *base = "http://a/b/c/d;p?q";
    RESOLVES(base, "g", "http://a/b/c/g");
    RESOLVES(base, "./g", "http://a/b/c/g");
    RESOLVES(base, "g/", "http://a/b/c/g/");
    RESOLVES(base, "/g", "http://a/g");
    RESOLVES(base, "//g", "http://g");
    RESOLVES(base, "?y", "http://a/b/c/d;p?y");
    RESOLVES(base, "g?y", "http://a/b/c/g?y");
    RESOLVES(base, "#s", "http://a/b/c/d;p?q");
    RESOLVES(base, "g#s", "http://a/b/c/g");
    RESOLVES(base, "g?y#s", "http://a/b/c/g?y");
    RESOLVES(base, ";x", "http://a/b/c/;x");
    RESOLVES(base, "g;x", "http://a/b/c/g;x");
    RESOLVES(base, ".", "http://a/b/c/");
    RESOLVES(base, "./", "http://a/b/c/");
    RESOLVES(base, "..", "http://a/b/");
    RESOLVES(base, "../", "http://a/b/");
    RESOLVES(base, "../g", "http://a/b/g");
    RESOLVES(base, "../..", "http://a/");
    RESOLVES(base, "../../", "http://a/");
    RESOLVES(base, "../../g", "http://a/g");
    RESOLVES(base, "../../../g", "http://a/g");
    RESOLVES(base, "../../../../g", "http://a/g");
    RESOLVES(base, "/./g", "http://a/g");
    RESOLVES(base, "/../g", "http://a/g");
    RESOLVES(base, "g.", "http://a/b/c/g.");
    RESOLVES(base, ".g", "http://a/b/c/.g");
    RESOLVES(base, "g..", "http://a/b/c/g..");
    RESOLVES(base, "..g", "http://a/b/c/..g");
    RESOLVES(base, "./../g", "http://a/b/g");
    RESOLVES(base, "./g/.", "http://a/b/c/g/");
    RESOLVES(base, "g/./h", "http://a/b/c/g/h");
    RESOLVES(base, "g/../h", "http://a/b/c/h");
    RESOLVES(base, "g;x=1/./y", "http://a/b/c/g;x=1/y");
    RESOLVES(base, "g;x=1/../y", "http://a/b/c/y");
    RESOLVES(base, "g?y/./x", "http://a/b/c/g?y/./x");
    RESOLVES(base, "g?y/../x", "http://a/b/c/g?y/../x");

    RESOLVES("http://h:8000/a/b", "http://other/x", "http://other/x");
    RESOLVES("http://h:8000/a/b", "HTTPS://other/x", "");          /* resolved, then refused */
    RESOLVES("http://h:8000/a/b", "next.html", "http://h:8000/a/next.html");
    RESOLVES("http://h:8000/a/b", "//cdn.example/file", "http://cdn.example/file");
    RESOLVES("http://h:8000/a/b?x", "?y", "http://h:8000/a/b?y");
    RESOLVES("http://h:8000", "next", "http://h:8000/next");
    RESOLVES("http://h:8000", "?y", "http://h:8000/?y");
    RESOLVES("h", "/x", "h/x");
    RESOLVES("h", "", "");

    /* Too long for the buffer: refused, the URL as it was. */
    char ref[200];
    memset(ref, 'x', sizeof ref - 1);
    ref[sizeof ref - 1] = 0;
    CHECK_EQ_INT(parse("http://h/a"), 0);
    CHECK_EQ_INT(url_resolve(url, sizeof url, &u, ref), -1);
    CHECK(!strcmp(url, "http://h/a"));
    ref[sizeof url - 1 - 8] = 0;                 /* "http://h/" + it: exactly full */
    CHECK_EQ_INT(url_resolve(url, sizeof url, &u, ref), -1);
    ref[sizeof url - 1 - 9] = 0;
    CHECK_EQ_INT(url_resolve(url, sizeof url, &u, ref), 0);
    CHECK_EQ_INT(strlen(url), sizeof url - 1);

    uint32_t v;
    const char *e = url_number("123", 10, &v);
    CHECK(e && !*e && v == 123);
    e = url_number("4294967295 ", 10, &v);
    CHECK(e && *e == ' ' && v == 4294967295u);
    CHECK(!url_number("4294967296", 10, &v));
    CHECK(!url_number("99999999999999999999", 10, &v));
    CHECK(!url_number("", 10, &v));
    CHECK(!url_number("x1", 10, &v));
    CHECK(!url_number("-1", 10, &v));
    e = url_number("1aF;ext", 16, &v);
    CHECK(e && *e == ';' && v == 0x1af);
    e = url_number("ffffffff", 16, &v);
    CHECK(e && !*e && v == 0xffffffffu);
    CHECK(!url_number("100000000", 16, &v));
    e = url_number("12abc", 10, &v);
    CHECK(e && *e == 'a' && v == 12);
    CHECK(url_is_word("Chunked", "chunked") && url_is_word("chunked \t", "chunked"));
    CHECK(!url_is_word("chunked, gzip", "chunked") && !url_is_word("gzip, chunked", "chunked"));
    CHECK(!url_is_word("chunk", "chunked") && !url_is_word("compress", "chunked"));
    return check_summary("test_url");
}
