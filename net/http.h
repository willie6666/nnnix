/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NNNIX_HTTP_H
#define NNNIX_HTTP_H
#include <stdint.h>

#define HTTP_LINE 128
#define HTTP_PATH 64
#define HTTP_MAX_BODY (2048u * 1024u)

struct http_request {
    char path[HTTP_PATH];
    uint32_t length;
    unsigned char started, put, has_length, host, http11, expect;
};

/* One CRLF-stripped line at a time; 0 = more headers, 1 = complete,
 * negative = HTTP error status. The line is scratch storage. */
int http_parse_line(struct http_request *r, char *line);
void httpd_poll(void);              /* socket 4, on-demand worker, port 8080 */

#endif
