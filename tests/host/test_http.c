/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "check.h"
#include "../../net/http.h"
#include <string.h>

static struct http_request r;
static int line(const char *s)
{
    char buf[256];
    strcpy(buf, s);
    return http_parse_line(&r, buf);
}
static void reset(void) { memset(&r, 0, sizeof r); }

int main(void)
{
    reset(); CHECK_EQ_INT(line("GET /home/test.bin HTTP/1.0"), 0);
    CHECK(!strcmp(r.path, "/home/test.bin")); CHECK_EQ_INT(line(""), 1);
    reset(); CHECK_EQ_INT(line("PUT /home/sub/test.bin HTTP/1.1"), 0);
    CHECK_EQ_INT(line("Host: 198.18.0.2:8080"), 0);
    CHECK_EQ_INT(line("cOnTeNt-LeNgTh: \t1048576 \t"), 0); CHECK_EQ_INT(r.length, HTTP_MAX_BODY);
    CHECK_EQ_INT(line("Expect: 100-continue"), 0); CHECK_EQ_INT(r.expect, 1);
    CHECK_EQ_INT(line(""), 1); CHECK_EQ_INT(r.put, 1);
    const char *paths[] = { "/etc/rc", "/home", "/home/", "/home/../etc/rc", "/home/a/../../x",
        "/home/%2e%2e/etc/rc", "/home/./a", "/home//a", "/home/a/", "/home/a?x=1",
        "/home/a#b", "/home/a\\b", "/home/.http-3.tmp", "/home/sub/.http-4.tmp",
        "/home/12345678901234567890123456789012" };
    for (unsigned i = 0; i < sizeof paths / sizeof paths[0]; ++i) {
        char buf[200]; snprintf(buf, sizeof buf, "GET %s HTTP/1.0", paths[i]);
        reset(); CHECK_EQ_INT(line(buf), -403);
    }
    reset(); CHECK_EQ_INT(line("POST /home/a HTTP/1.0"), -405);
    reset(); CHECK_EQ_INT(line("GET /home/a HTTP/2.0"), -400);
    reset(); CHECK_EQ_INT(line("GET /home/a HTTP/1.1"), 0); CHECK_EQ_INT(line(""), -400);
    reset(); line("PUT /home/a HTTP/1.0"); CHECK_EQ_INT(line(""), -411);
    reset(); line("PUT /home/a HTTP/1.0");
    CHECK_EQ_INT(line("Content-Length: 0"), 0); CHECK_EQ_INT(line(""), 1);
    CHECK_EQ_INT(line("Content-Length: 0"), -400);
    const char *bad[] = { "Content-Length:", "Content-Length: -1", "Content-Length: +1",
        "Content-Length: 1, 1", "Content-Length: 1x", "Content-Length : 1", " Host: x", "NoColon",
        "Content-Range: bytes 0-1/5", "Host:", "Host: a b" };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        reset(); line("PUT /home/a HTTP/1.0"); CHECK_EQ_INT(line(bad[i]), -400);
    }
    reset(); line("PUT /home/a HTTP/1.0"); CHECK_EQ_INT(line("Content-Length: 1048577"), -413);
    CHECK_EQ_INT(line("Content-Length: 4294967296"), -413);
    reset(); line("PUT /home/a HTTP/1.0"); CHECK_EQ_INT(line("Transfer-Encoding: chunked"), -501);
    CHECK_EQ_INT(line("Expect: other"), -417);
    reset(); line("GET /home/a HTTP/1.0"); line("Content-Length: 1"); CHECK_EQ_INT(line(""), -400);
    reset(); line("GET /home/a HTTP/1.0"); line("Host: one"); CHECK_EQ_INT(line("Host: two"), -400);
    return check_summary("test_http");
}
