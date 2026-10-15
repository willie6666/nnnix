/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "nsh.h"
#include "cmds.h"
#include "shparse.h"
#include "../lib/stdio.h"
#include "../lib/string.h"
#include "../lib/sys.h"
#include "../kernel/task.h"          /* struct task_info layout */
#include "../kernel/kmem.h"          /* struct kmem_stats layout */

#define ENV_MAX 8
#define SCRIPT_MAX 3

struct shell {
    struct sh_command *cmd;        /* allocated only while executing a line */
    char line[SH_LINE];
    char *env[ENV_MAX];             /* NAME=VALUE strings, local to this shell */
    int scripts[SCRIPT_MAX], depth, status;
    char status_text[12];
};
static volatile int boot_done;
int nsh_boot_done(void) { return boot_done; }

struct command {
    const char *name;
    int (*run)(int argc, char **argv);
    const char *help;
};

static int cmd_help(int argc, char **argv);

static int cmd_echo(int argc, char **argv)
{
    for (int i = 1; i < argc; ++i) printf(i + 1 < argc ? "%s " : "%s", argv[i]);
    printf("\n");
    return 0;
}

static int cmd_ps(int argc, char **argv)
{
    static const char *const states[] = { "-", "run", "sleep", "wait", "zombie" };
    struct task_info info[4];        /* paged: the shell stack is small */
    (void)argc; (void)argv;
    printf("  PID NAME     PRI STATE  STACK USED/SIZE   CPU MS\n");
    for (int skip = 0, n; (n = sys_ps(info, 4, skip)) > 0; skip += n)
        for (int i = 0; i < n; ++i)
            printf("%5u %-8s %3u %-6s %10u/%-5u %8u\n", info[i].pid, info[i].name, info[i].prio,
                   info[i].state < 5 ? states[info[i].state] : "?", info[i].stack_used, info[i].stack_size,
                   info[i].cpu_ms);
    return 0;
}

static int cmd_free(int argc, char **argv)
{
    struct kmem_stats m;
    (void)argc; (void)argv;
    sys_meminfo(&m);
    printf("heap: total %u B, free %u B, largest block %u B, %u free blocks\n",
           m.total, m.free, m.largest, m.free_blocks);
    return 0;
}

static int cmd_uptime(int argc, char **argv)
{
    (void)argc; (void)argv;
    uint32_t ms = sys_uptime(), s = ms / 1000u;
    printf("up %u:%02u:%02u.%03u\n", s / 3600u, s / 60u % 60u, s % 60u, ms % 1000u);
    return 0;
}

static int cmd_dmesg(int argc, char **argv)
{
    char buf[96];
    uint32_t pos = 0;
    int n;
    (void)argc; (void)argv;
    while ((n = sys_klog(&pos, buf, sizeof buf)) > 0) sys_write(1, buf, (size_t)n);
    return 0;
}

static int cmd_uname(int argc, char **argv)
{
    char buf[48];
    (void)argc; (void)argv;
    sys_uname(buf, sizeof buf);
    printf("%s\n", buf);
    return 0;
}

static int cmd_sleep(int argc, char **argv)
{
    uint32_t s = 0;
    if (argc != 2 || !argv[1][0]) { printf("usage: sleep SECONDS\n"); return 1; }
    for (const char *p = argv[1]; *p; ++p) {
        if (*p < '0' || *p > '9' || s > (2147483u - (uint32_t)(*p - '0')) / 10u) {
            printf("sleep: invalid duration\n"); return 1;
        }
        s = s * 10u + (uint32_t)(*p - '0');
    }
    sys_sleep(s * 1000u);
    return 0;
}

static int cmd_clear(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("\033[H\033[2J");
    return 0;
}

static int cmd_reboot(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("rebooting\n");
    sys_sleep(100);                  /* let the line reach the terminal */
    sys_reboot();
    return 1;
}

static const struct command commands[] = {
    { "help", cmd_help, "list commands" },
    { "echo", cmd_echo, "print arguments" },
    { "ps", cmd_ps, "list tasks" },
    { "free", cmd_free, "kernel heap usage" },
    { "uptime", cmd_uptime, "time since boot" },
    { "dmesg", cmd_dmesg, "kernel log" },
    { "uname", cmd_uname, "system name and version" },
    { "sleep", cmd_sleep, "sleep SECONDS" },
    { "ls", cmd_ls, "list directory" },
    { "cat", cmd_cat, "print files" },
    { "cp", cmd_cp, "copy a file" },
    { "mv", cmd_mv, "move or rename" },
    { "rm", cmd_rm, "remove files" },
    { "mkdir", cmd_mkdir, "make directories" },
    { "rmdir", cmd_rmdir, "remove empty dirs" },
    { "touch", cmd_touch, "create empty files" },
    { "pwd", cmd_pwd, "working directory" },
    { "cd", cmd_cd, "change directory" },
    { "df", cmd_df, "file system space" },
    { "mkfile", cmd_mkfile, "NAME SIZE test file" },
    { "sum", cmd_sum, "CRC-32 of files" },
    { "wc", cmd_wc, "count lines, words, bytes" },
    { "head", cmd_head, "first lines (-n COUNT)" },
    { "tail", cmd_tail, "last lines (-n COUNT)" },
    { "grep", cmd_grep, "literal text search" },
    { "hexdump", cmd_hexdump, "hexadecimal and ASCII" },
    { "sh", 0, "run a script in this shell" },
    { "source", 0, "run a script in this shell" },
    { "set", 0, "set NAME VALUE" },
    { "unset", 0, "remove a variable" },
    { "env", 0, "list shell variables" },
    { "clear", cmd_clear, "clear the screen" },
    { "reboot", cmd_reboot, "restart the board" },
    { "exit", 0, "leave the shell" },
};

static int cmd_help(int argc, char **argv)
{
    (void)argc; (void)argv;
    for (unsigned i = 0; i < sizeof commands / sizeof commands[0]; ++i)
        printf("  %-8s %s\n", commands[i].name, commands[i].help);
    return 0;
}

static const char *lookup(void *ctx, const char *name)
{
    struct shell *s = ctx;
    if (!strcmp(name, "?")) {
        unsigned v = (unsigned)s->status, n = 0;
        do { s->status_text[n++] = (char)('0' + v % 10u); v /= 10u; } while (v && n < 10);
        s->status_text[n] = 0;
        for (unsigned i = 0; i < n / 2; ++i) {
            char c = s->status_text[i]; s->status_text[i] = s->status_text[n - i - 1]; s->status_text[n - i - 1] = c;
        }
        return s->status_text;
    }
    size_t n = strlen(name);
    for (int i = 0; i < ENV_MAX; ++i)
        if (s->env[i] && !strncmp(s->env[i], name, n) && s->env[i][n] == '=') return s->env[i] + n + 1;
    return "";
}

static int variable(struct shell *s, int argc, char **argv)
{
    if (!strcmp(argv[0], "env") || (argc == 1 && !strcmp(argv[0], "set"))) {
        for (int i = 0; i < ENV_MAX; ++i) if (s->env[i]) printf("%s\n", s->env[i]);
        return 0;
    }
    int unset = !strcmp(argv[0], "unset");
    if (argc != (unset ? 2 : 3)) { printf("usage: %s NAME%s\n", argv[0], unset ? "" : " VALUE"); return 1; }
    size_t n = strlen(argv[1]);
    if (!n || n > 31) { printf("nsh: invalid variable name\n"); return 1; }
    for (size_t i = 0; i < n; ++i)
        if (!sh_name_char(argv[1][i], i == 0)) { printf("nsh: invalid variable name\n"); return 1; }
    int slot = -1;
    for (int i = 0; i < ENV_MAX; ++i) {
        if (!s->env[i]) { if (slot < 0) slot = i; }
        else if (!strncmp(s->env[i], argv[1], n) && s->env[i][n] == '=') { slot = i; break; }
    }
    if (unset) {
        if (slot >= 0 && s->env[slot] && !strncmp(s->env[slot], argv[1], n) && s->env[slot][n] == '=') {
            sys_free(s->env[slot]); s->env[slot] = 0;
        }
        return 0;
    }
    if (slot < 0) { printf("nsh: variable table full\n"); return 1; }
    char *v = sys_alloc(n + strlen(argv[2]) + 2);
    if (!v) { printf("nsh: out of memory\n"); return 1; }
    memcpy(v, argv[1], n); v[n] = '='; strcpy(v + n + 1, argv[2]);
    sys_free(s->env[slot]); s->env[slot] = v;
    return 0;
}

static int push_script(struct shell *s, const char *path, int optional)
{
    if (s->depth == SCRIPT_MAX) { printf("nsh: scripts nested too deeply\n"); return 1; }
    int fd = sys_open(path, O_RDONLY);
    if (fd < 0) {
        if (!optional || fd != SYS_ENOENT) printf("nsh: %s: %s\n", path, errstr(fd));
        return optional && fd == SYS_ENOENT ? 0 : 1;
    }
    s->scripts[s->depth++] = fd;
    return 0;
}

/* Read the entire script line before execution. Overlong or binary lines are
 * drained and rejected, so a truncated prefix never becomes a command. */
static int script_line(struct shell *s)
{
    int n = 0, bad = 0, got = 0;
    for (;;) {
        char c;
        int e = sys_read(s->scripts[s->depth - 1], &c, 1);
        if (e < 0) return e;
        if (!e || c == '\n') { if (e) got = 1; break; }
        got = 1;
        if (!c || n >= SH_LINE - 1) bad = 1;
        else s->line[n++] = c;
    }
    s->line[n] = 0;
    return bad ? SYS_EINVAL : got;
}

/* Returns one to leave the current input source. Scripts deliberately share
 * cwd and variables with their caller; no recursive C calls or extra stacks. */
static int execute_line(struct shell *s)
{
    const char *error = sh_parse(s->line, s->cmd, lookup, s);
    if (error) { printf("nsh: %s\n", error); s->status = 2; return 0; }
    int argc = s->cmd->argc;
    char **argv = s->cmd->argv;
    if (!argc) return 0;
    int script = !strcmp(argv[0], "sh") || !strcmp(argv[0], "source") || !strcmp(argv[0], ".");
    if (script) {
        if (argc != 2 || s->cmd->nredir) { printf("usage: sh FILE (without redirection)\n"); s->status = 2; }
        else s->status = push_script(s, argv[1], 0);
        return 0;
    }
    if (!strcmp(argv[0], "exit") || !strcmp(argv[0], "logout")) return 1;
    const struct command *c = 0;
    for (unsigned i = 0; i < sizeof commands / sizeof commands[0]; ++i)
        if (!strcmp(argv[0], commands[i].name)) c = &commands[i];
    if (!c) { printf("nsh: %s: command not found\n", argv[0]); s->status = 127; return 0; }
    s->status = 0;
    for (int i = 0; i < s->cmd->nredir; ++i) {
        struct sh_redir *r = &s->cmd->redir[i];
        int flags = r->target ? O_WRONLY | O_CREAT | (r->append ? O_APPEND : O_TRUNC) : O_RDONLY;
        int fd = sys_open(r->path, flags);
        if (fd < 0) { printf("nsh: %s: %s\n", r->path, errstr(fd)); s->status = 1; break; }
        int e = sys_dup2(fd, r->target);
        sys_close(fd);
        if (e < 0) { s->status = 1; break; }
    }
    if (!s->status) s->status = c->run ? c->run(argc, argv) : variable(s, argc, argv);
    sys_dup2(-1, 0); sys_dup2(-1, 1);
    return 0;
}

static int execute(struct shell *s)
{
    s->cmd = sys_alloc(sizeof *s->cmd);
    if (!s->cmd) { printf("nsh: out of memory\n"); s->status = 1; return 0; }
    int leave = execute_line(s);
    sys_free(s->cmd); s->cmd = 0;
    return leave;
}

static void run_shell(int interactive)
{
    struct shell *s = sys_alloc(sizeof *s);
    if (!s) { printf("nsh: out of memory\n"); return; }
    memset(s, 0, sizeof *s);
    if (interactive) {
        sys_uname(s->line, sizeof s->line);
        printf("\n%s\nType 'help' for a list of commands.\n", s->line);
    } else push_script(s, "/etc/rc", 1);
    while (interactive || s->depth) {
        int scripted = s->depth > 0;
        if (scripted) {
            int n = script_line(s);
            if (n <= 0) {
                if (n < 0) { printf("nsh: script read failed: %s\n", errstr(n)); s->status = 2; }
                sys_close(s->scripts[--s->depth]);
                continue;
            }
        } else {
            printf("nnnix$ ");
            int n = sys_read(0, s->line, sizeof s->line - 1);
            if (!n) break;
            if (n < 0) continue;
            s->line[n] = 0;
        }
        if (execute(s)) {
            if (scripted) sys_close(s->scripts[--s->depth]);
            else break;
        }
    }
    for (int i = 0; i < s->depth; ++i) sys_close(s->scripts[i]);
    for (int i = 0; i < ENV_MAX; ++i) sys_free(s->env[i]);
    sys_free(s);
    if (interactive) printf("logout\n");
}

void nsh_main(void) { run_shell(1); }
void nsh_boot(void *arg) { (void)arg; run_shell(0); boot_done = 1; }
