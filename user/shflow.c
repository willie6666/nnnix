/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Bounded control stack; loops reread their condition from a seekable file.
 * Classify raw reserved words before expansion, so skipped branches do not
 * expand variables, open redirections, execute commands or source files. */
#include "shflow.h"
#include "../lib/string.h"

enum { IF_WAIT = 1, IF_BODY, IF_ELSE, WHILE_WAIT, WHILE_BODY, WHILE_AGAIN };
enum { ACTIVE = 1, TAKEN = 2, BREAK = 4, CONTINUE = 8 };
enum { IF = 1, THEN, ELIF, ELSE, FI, WHILE, DO, DONE, BRK, CONT };

static int blank(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
static const char *space(const char *p) { while (blank(*p)) ++p; return p; }
static int empty(const char *p) { p = space(p); return !*p || *p == '#'; }

int sh_control_word(const char *p)
{
    static const char *const names[] = { "if", "then", "elif", "else", "fi", "while", "do", "done", "break", "continue" };
    p = space(p);
    for (unsigned i = 0; i < sizeof names / sizeof *names; ++i) {
        size_t n = strlen(names[i]);
        if (!strncmp(p, names[i], n) && (!p[n] || blank(p[n]))) return (int)i + 1;
    }
    return 0;
}

static int active(struct sh_flow *f, unsigned end)
{
    for (unsigned i = 0; i < end; ++i)
        if (!(f->blocks[i].flags & ACTIVE) || (f->blocks[i].flags & (BREAK | CONTINUE))) return 0;
    return 1;
}

void sh_flow_record(struct sh_flow *f, unsigned base, int status)
{
    if (!active(f, f->depth)) return;
    for (unsigned i = base; i < f->depth; ++i) f->blocks[i].result = status;
}

/* Optional "; then"/"; do" after a condition. Quotes/escapes and comments
 * follow shparse. Other semicolon lists remain unsupported. */
static int header(char *p, const char *ending)
{
    char quote = 0;
    int word = 0;
    for (; *p; ++p) {
        if (*p == '\\' && quote != '\'') { if (p[1]) ++p; word = 1; continue; }
        if ((*p == '\'' || *p == '"') && (!quote || quote == *p)) {
            quote = quote ? 0 : *p; word = 1; continue;
        }
        if (quote) continue;
        if (*p == '#' && !word) break;
        if (*p == ';') {
            const char *tail = space(p + 1);
            size_t n = strlen(ending);
            if (strncmp(tail, ending, n) || (tail[n] && !blank(tail[n])) || !empty(tail + n)) return -1;
            *p = 0;
            return 1;
        }
        word = blank(*p) || *p == '|' || *p == '&' || *p == '<' || *p == '>' ? 0 : 1;
    }
    return 0;
}

const char *sh_flow_line(struct sh_flow *f, unsigned base, char *line, uint32_t offset,
                         int *status, const struct sh_flow_ops *ops, void *ctx, int *leave)
{
    *leave = 0;
    if (empty(line)) return 0;
    int word = sh_control_word(line);
    char *tail = (char *)space(line);
    while (*tail && !blank(*tail)) ++tail;
    tail = (char *)space(tail);
    struct sh_block *b = f->depth > base ? &f->blocks[f->depth - 1] : 0;
    if (b && b->state == WHILE_AGAIN && (word != WHILE || offset != b->offset)) return "loop source changed";
    if (b && (b->state == IF_WAIT || b->state == WHILE_WAIT) && word != (b->state == IF_WAIT ? THEN : DO))
        return b->state == IF_WAIT ? "expected then" : "expected do";
    if (word == IF || word == WHILE || word == ELIF) {
        if (word == ELIF && (!b || b->state != IF_BODY)) return "unexpected elif";
        int inline_body = header(tail, word == WHILE ? "do" : "then");
        if (inline_body < 0) return "expected ; then or ; do at end of condition";
        if (empty(tail)) return "missing condition";
        if (word != ELIF && !(b && b->state == WHILE_AGAIN)) {
            if (f->depth == SH_BLOCKS) return "control nesting too deep";
            b = &f->blocks[f->depth++];
            *b = (struct sh_block){ .offset = offset };
        }
        if (word == WHILE && offset > 0x7fffffffu) return "while needs a seekable script";
        int enabled = active(f, f->depth - 1) && (word != ELIF || !(b->flags & TAKEN));
        b->flags &= TAKEN;
        if (enabled) {
            if (ops->run(ctx, tail, 1)) { *leave = 1; return 0; }
            if (!*status) b->flags |= ACTIVE | TAKEN;
        }
        b->state = (uint8_t)(word == WHILE ? (inline_body ? WHILE_BODY : WHILE_WAIT) : (inline_body ? IF_BODY : IF_WAIT));
        return 0;
    }
    if (word >= THEN && word <= DONE && word != WHILE) {
        if (!empty(tail)) return "unexpected arguments after control word";
        if (!b) return "unexpected control word";
        if (word == THEN && b->state == IF_WAIT) { b->state = IF_BODY; return 0; }
        if (word == DO && b->state == WHILE_WAIT) { b->state = WHILE_BODY; return 0; }
        if (word == ELSE && b->state == IF_BODY) {
            b->state = IF_ELSE;
            b->flags = (uint8_t)((b->flags & TAKEN) | (!(b->flags & TAKEN) && active(f, f->depth - 1) ? ACTIVE : 0));
            return 0;
        }
        if ((word == FI && (b->state == IF_BODY || b->state == IF_ELSE)) || (word == DONE && b->state == WHILE_BODY)) {
            if (word == DONE && active(f, f->depth - 1) && (b->flags & ACTIVE) && !(b->flags & BREAK)) {
                b->state = WHILE_AGAIN;
                if (ops->seek(ctx, b->offset)) return "cannot seek loop condition";
                return 0;
            }
            --f->depth;
            if (active(f, f->depth)) { *status = b->result; sh_flow_record(f, base, *status); }
            return 0;
        }
        return "unexpected control word";
    }
    if (word == BRK || word == CONT) {
        if (!empty(tail)) return "usage: break | continue (without arguments)";
        if (!active(f, f->depth)) return 0;
        for (unsigned i = f->depth; i > base; --i) if (f->blocks[i - 1].state == WHILE_BODY) {
            *status = 0;
            sh_flow_record(f, base, 0);
            f->blocks[i - 1].flags |= (uint8_t)(word == BRK ? BREAK : CONTINUE);
            return 0;
        }
        return "break/continue outside a loop";
    }
    if (active(f, f->depth)) {
        *leave = ops->run(ctx, line, 0);
        sh_flow_record(f, base, *status);
    }
    return 0;
}
