/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "nsh.h"
#include "../lib/stdio.h"
#include "../lib/string.h"
#include "../lib/sys.h"
#include "../kernel/task.h"          /* struct task_info layout */
#include "../kernel/kmem.h"          /* struct kmem_stats layout */

#define MAX_ARGS 8

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
    struct task_info info[TASK_MAX];
    (void)argc; (void)argv;
    int n = sys_ps(info, TASK_MAX);
    printf("  PID NAME     PRI STATE  STACK USED/SIZE   CPU MS\n");
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
    if (argc != 2) { printf("usage: sleep SECONDS\n"); return 1; }
    for (const char *p = argv[1]; *p >= '0' && *p <= '9'; ++p) s = s * 10u + (uint32_t)(*p - '0');
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

/* Split on blanks; double quotes group words. Returns argc. */
static int split(char *line, char **argv)
{
    int argc = 0;
    char *p = line;
    while (*p && argc < MAX_ARGS) {
        while (*p == ' ' || *p == '\t') ++p;
        if (!*p) break;
        if (*p == '"') {
            argv[argc++] = ++p;
            while (*p && *p != '"') ++p;
        } else {
            argv[argc++] = p;
            while (*p && *p != ' ' && *p != '\t') ++p;
        }
        if (*p) *p++ = 0;
    }
    return argc;
}

void nsh_main(void)
{
    char line[96], *argv[MAX_ARGS];
    char name[48];
    sys_uname(name, sizeof name);
    printf("\n%s\nType 'help' for a list of commands.\n", name);
    for (;;) {
        printf("nnnix$ ");
        int n = sys_read(0, line, sizeof line - 1);
        if (n == 0) break;                            /* end of file or hang-up */
        if (n < 0) continue;                          /* Ctrl-C */
        line[n] = 0;
        if (n && line[n - 1] == '\n') line[n - 1] = 0;
        int argc = split(line, argv);
        if (!argc) continue;
        if (!strcmp(argv[0], "exit") || !strcmp(argv[0], "logout")) break;
        const struct command *c = 0;
        for (unsigned i = 0; i < sizeof commands / sizeof commands[0]; ++i)
            if (!strcmp(argv[0], commands[i].name)) c = &commands[i];
        if (c && c->run) c->run(argc, argv);
        else printf("nsh: %s: command not found\n", argv[0]);
    }
    printf("logout\n");
}
