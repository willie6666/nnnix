/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NNNIX_URL_H
#define NNNIX_URL_H
#include <stddef.h>
#include <stdint.h>

/* HTTP URLs and header numbers for wget (bin/url.c). */
#define URL_HOST_MAX 64

struct url {
    char host[URL_HOST_MAX];
    uint16_t port;
    uint16_t path;                       /* where the path starts in the URL ("" or "?q" when it has none) */
};

/* [http://]host[:port][/path][?query][#fragment], in place: the fragment is
 * cut off and "." and ".." segments taken out of the path. 0, -1 when it is
 * not one, or -2 for an https:// URL. */
int url_parse(char *url, struct url *u);

/* A reference (a redirect's Location: an absolute URL, //host/path, /path,
 * path, ?query, #fragment) against url as url_parse left it, into url
 * (size bytes) in place. 0, or -1 when it is empty or does not fit. */
int url_resolve(char *url, size_t size, const struct url *u, const char *ref);

/* A decimal (base 10) or hex (16) number at s: the character after its last
 * digit, or 0 when there is no digit or it does not fit in 32 bits. */
const char *url_number(const char *s, int base, uint32_t *v);

/* Spaces and tabs at s skipped: where they end. */
const char *url_skip_blanks(const char *s);

/* s equals word ignoring case, blanks after it allowed. */
int url_is_word(const char *s, const char *word);

#endif
