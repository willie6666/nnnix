/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "shparse.h"

int sh_name_char(char c, int first)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'
        || (!first && c >= '0' && c <= '9');
}

static int blank(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
static int special(char c) { return c == '<' || c == '>' || c == '|' || c == '&' || c == ';'; }

/* One command, up to the end of the line or a control operator (| & ;),
 * which is stored in *term (0 at the end) and left unconsumed. */
struct target { char *words; int *used; char **argv; struct sh_redir *redir; int *argc, *nredir; };

static const char *parse_one(const char **pp, struct target *o, char *term,
                             const char *(*lookup)(void *, const char *), void *ctx)
{
    const char *p = *pp;
    int used = *o->used, pending = -1;
    *o->argc = *o->nredir = 0;
    o->argv[0] = 0;
    *term = 0;
    for (;;) {
        while (blank(*p)) ++p;
        if (!*p || *p == '#') break;
        if (special(*p) && *p != '<' && *p != '>') { *term = *p; break; }
        if (special(*p)) {
            char op = *p++;
            if (pending >= 0) return "missing redirection file";
            if (*o->nredir == SH_REDIRS) return "too many redirections";
            int append = op == '>' && *p == '>';
            if (append) ++p;
            if (*p == op) return "unsupported redirection";
            pending = (*o->nredir)++;
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
            if (*o->argc == SH_ARGS - 1) return "too many arguments";
            o->argv[(*o->argc)++] = word;
            o->argv[*o->argc] = 0;
        }
    }
    *pp = p;
    *o->used = used;
    return pending >= 0 ? "missing redirection file" : 0;
}

const char *sh_parse(const char *p, struct sh_command *o,
                     const char *(*lookup)(void *, const char *), void *ctx)
{
    char term;
    int used = 0;
    struct target t = { o->words, &used, o->argv, o->redir, &o->argc, &o->nredir };
    const char *e = parse_one(&p, &t, &term, lookup, ctx);
    if (e) return e;
    return term ? "pipes, jobs and command lists are not supported here" : 0;
}

const char *sh_parse_line(const char *p, struct sh_pipeline *pl,
                          const char *(*lookup)(void *, const char *), void *ctx)
{
    int used = 0;
    pl->count = pl->background = 0;
    for (;;) {
        char term;
        if (pl->count == SH_STAGES) return "pipeline too long";
        struct sh_stage *st = &pl->cmd[pl->count];
        struct target t = { pl->words, &used, st->argv, st->redir, &st->argc, &st->nredir };
        const char *e = parse_one(&p, &t, &term, lookup, ctx);
        if (e) return e;
        if (st->argc) ++pl->count;
        else if (term || pl->count) return "missing command";   /* "| x", "x |", "&" */
        if (!term) return 0;
        ++p;                                   /* consume the operator */
        if (term == ';') return "command lists are not supported";
        if (term == '&') {
            while (blank(*p)) ++p;
            if (*p && *p != '#') return "& must end the line";
            pl->background = 1;
            return 0;
        }
        if (*p == '|') return "|| is not supported";
    }
}
