/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NNNIX_SHPARSE_H
#define NNNIX_SHPARSE_H

#define SH_LINE 128
#define SH_WORDS 192
#define SH_ARGS 12
#define SH_REDIRS 4

struct sh_redir { char *path; unsigned char target, append; };
struct sh_command {
    char words[SH_WORDS];
    char *argv[SH_ARGS];             /* includes the terminating null pointer */
    struct sh_redir redir[SH_REDIRS];
    int argc, nredir;
};

/* Quotes, escapes, comments, variables and ordered < > >> redirections.
 * Expansions stay in one word; no globbing or field splitting. The lookup
 * callback returns an empty string for an unset variable. */
const char *sh_parse(const char *line, struct sh_command *out,
                     const char *(*lookup)(void *, const char *), void *ctx);
int sh_name_char(char c, int first);

#endif
