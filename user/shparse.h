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
 * Expansions stay in one word; standalone $@/"$@" expands the positional list
 * without field splitting. $0-$9 and $# (also braced) use the lookup callback.
 * The lookup
 * callback returns an empty string for an unset variable. */
const char *sh_parse(const char *line, struct sh_command *out,
                     const char *(*lookup)(void *, const char *), void *ctx);
/* A pipeline of up to SH_STAGES commands separated by '|', optionally ending
 * in '&'. The stages share one word buffer. */
#define SH_STAGES 3
struct sh_stage {
    char *argv[SH_ARGS];
    struct sh_redir redir[SH_REDIRS];
    int argc, nredir;
};
struct sh_pipeline {
    char words[SH_WORDS];
    struct sh_stage cmd[SH_STAGES];
    int count, background;
};
const char *sh_parse_line(const char *line, struct sh_pipeline *pl,
                          const char *(*lookup)(void *, const char *), void *ctx);
int sh_name_char(char c, int first);

#endif
