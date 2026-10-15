/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "shparse.h"

int sh_name_char(char c, int first)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'
        || (!first && c >= '0' && c <= '9');
}

static int blank(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
static int special(char c) { return c == '<' || c == '>' || c == '|' || c == '&' || c == ';'; }

const char *sh_parse(const char *p, struct sh_command *o,
                    const char *(*lookup)(void *, const char *), void *ctx)
{
    int used = 0, pending = -1;
    o->argc = o->nredir = 0;
    o->argv[0] = 0;
    for (;;) {
        while (blank(*p)) ++p;
        if (!*p || *p == '#') break;
        if (special(*p)) {
            char op = *p++;
            if (op != '<' && op != '>') return "pipes, jobs and command lists are not supported yet";
            if (pending >= 0) return "missing redirection file";
            if (o->nredir == SH_REDIRS) return "too many redirections";
            int append = op == '>' && *p == '>';
            if (append) ++p;
            if (*p == op) return "unsupported redirection";
            pending = o->nredir++;
            o->redir[pending].target = (unsigned char)(op == '>');
            o->redir[pending].append = (unsigned char)append;
            continue;
        }
        char *word = o->words + used;
        char quote = 0;
        while (*p && (quote || (!blank(*p) && !special(*p)))) {
            char c = *p++;
            if ((c == '\'' || c == '"') && (!quote || quote == c)) {
                quote = quote ? 0 : c;
                continue;
            }
            if (c == '\\' && quote != '\'') {
                if (!*p) return "trailing escape";
                c = *p++;
            } else if (c == '$' && quote != '\'') {
                char name[32];
                int n = 0, brace = *p == '{';
                if (brace) ++p;
                if (*p == '?' && !brace) { name[n++] = *p++; }
                else while (sh_name_char(*p, n == 0)) {
                    if (n == (int)sizeof name - 1) return "variable name too long";
                    name[n++] = *p++;
                }
                if (brace && (!n || *p++ != '}')) return "bad variable expansion";
                if (n) {
                    name[n] = 0;
                    const char *v = lookup ? lookup(ctx, name) : "";
                    while (v && *v) {
                        if (used >= SH_WORDS - 1) return "expanded line too long";
                        o->words[used++] = *v++;
                    }
                    continue;
                }
            }
            if (used >= SH_WORDS - 1) return "expanded line too long";
            o->words[used++] = c;
        }
        if (quote) return "unclosed quote";
        if (used >= SH_WORDS) return "expanded line too long";
        o->words[used++] = 0;
        if (pending >= 0) {
            if (!*word) return "empty redirection file";
            o->redir[pending].path = word;
            pending = -1;
        } else {
            if (o->argc == SH_ARGS - 1) return "too many arguments";
            o->argv[o->argc++] = word;
            o->argv[o->argc] = 0;
        }
    }
    return pending >= 0 ? "missing redirection file" : 0;
}
