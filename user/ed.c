/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ed: the shell command around the editor core (edcore.c). */
#include "cmds.h"
#include "ed.h"
#include "../lib/stdio.h"
#include "../lib/string.h"
#include "../lib/sys.h"

#define LINE 128

struct session {
    struct ed ed;
    char in[LINE], line[LINE];
    uint16_t pos, len;
};

void ed_out(const char *s, uint32_t n) { sys_write(1, s, n); }
void *ed_alloc(uint32_t n) { return sys_alloc(n); }
void ed_release(void *p) { if (p) sys_free(p); }
int ed_grow(void *p, uint32_t n) { return sys_grow(p, n); }
int ed_open(const char *path, int write) { return sys_open(path, write ? O_WRONLY | O_CREAT | O_TRUNC : O_RDONLY); }
int ed_read(int fd, void *buf, uint32_t n) { return sys_read(fd, buf, n); }
int ed_write(int fd, const void *buf, uint32_t n) { return sys_write(fd, buf, n); }
int ed_close(int fd) { return sys_close(fd); }

/* Next input line without its '\n': length, -1 at end of input, -2 on Ctrl-C.
 * Overlong lines are cut. A last line without '\n' counts; one cut short by a
 * read error does not (it could be a command with half a file name). */
static int next_line(struct session *s)
{
    int n = 0;
    for (;;) {
        if (s->pos == s->len) {
            int r = sys_read(0, s->in, sizeof s->in);
            if (r == SYS_EINTR) return -2;
            if (r < 0 || (r == 0 && !n)) return -1;
            if (r == 0) break;
            s->pos = 0;
            s->len = (uint16_t)r;
        }
        char c = s->in[s->pos++];
        if (c == '\n') break;
        if (c != '\r' && n < LINE - 1) s->line[n++] = c;
    }
    s->line[n] = 0;
    return n;
}

int cmd_ed(int argc, char **argv)
{
    int silent = 0, i = 1;
    if (i < argc && !strcmp(argv[i], "-s")) { silent = 1; ++i; }
    if (argc - i > 1 || (i < argc && strlen(argv[i]) >= ED_PATH)) {
        printf("usage: ed [-s] [FILE]\n");
        return 1;
    }
    struct session *s = sys_alloc(sizeof *s);
    if (!s) { printf("ed: %s\n", errstr(SYS_ENOMEM)); return 1; }
    memset(s, 0, sizeof *s);
    ed_init(&s->ed, silent);
    if (i < argc) {
        strcpy(s->ed.file, argv[i]);
        int n = ed_load(&s->ed, argv[i], 0);
        s->ed.dirty = 0;
        if (n == SYS_ENOENT) { if (!silent) printf("%s: new file\n", argv[i]); }
        else if (n < 0) printf("? %s: %s\n", argv[i], errstr(n));
        else if (!silent) printf("%d\n", n);
    }
    int ended = 0;
    for (;;) {
        if (!silent && s->ed.input_at < 0) sys_write(1, "*", 1);
        int n = next_line(s);
        if (n == -2) { printf("?\n"); continue; }
        if (n == -1) {
            if (s->ed.input_at >= 0) { ed_line(&s->ed, "."); continue; }
            if (!silent) printf("\n");
            if (ended++ || ed_line(&s->ed, "q") == ED_QUIT) break;
            continue;
        }
        if (ed_line(&s->ed, s->line) == ED_QUIT) break;
    }
    int rc = s->ed.dirty ? 1 : 0;
    ed_free_buffer(&s->ed);
    sys_free(s);
    return rc;
}
