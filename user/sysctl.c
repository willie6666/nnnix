/* SPDX-License-Identifier: GPL-3.0-or-later */
/* sysctl: run-time settings, most of which give RAM back while a feature is unused. */
#include "cmds.h"
#include "../lib/stdio.h"
#include "../lib/string.h"
#include "../lib/sys.h"

enum { KIND_CONSOLE, KIND_NUMBER, KIND_SERVICE, KIND_FONT };

struct setting {
    const char *name;
    uint8_t kind, key;               /* key: NN_CTL_* or an NN_SVC_* bit */
    const char *help;
};

static const struct setting settings[] = {
    { "console", KIND_CONSOLE, NN_CTL_CONSOLE, "off|ask|on: LCD shell (1.5 KiB while it runs)" },
    { "console_font", KIND_FONT, NN_CTL_CONSOLE_FONT, "normal|small: LCD text 21x8 or 32x10 (Ctrl+= / Ctrl+-)" },
    { "console_history", KIND_NUMBER, NN_CTL_CONSOLE_HISTORY, "bytes of LCD scrollback (PageUp/PageDown), 0-8192" },
    { "idle_logout", KIND_NUMBER, NN_CTL_IDLE_LOGOUT, "seconds without input before logout; 0 never" },
    { "telnet_max", KIND_NUMBER, NN_CTL_TELNET_MAX, "telnet sessions at once, 0-3 (1.5 KiB each)" },
    { "watchdog", KIND_NUMBER, NN_CTL_WATCHDOG, "reset after 0 (off), 2, 7 or 26 s without the kernel daemon" },
    { "telnetd", KIND_SERVICE, NN_SVC_TELNETD, "on|off: accept telnet logins" },
    { "httpd", KIND_SERVICE, NN_SVC_HTTPD, "on|off: HTTP file transfer on port 8080" },
};
#define COUNT (sizeof settings / sizeof settings[0])

static const char *const console_modes[] = { "off", "ask", "on" };
static const char *const fonts[] = { "normal", "small" };

static void show(const struct setting *s)
{
    if (s->kind == KIND_SERVICE) {
        struct nn_netcfg c;
        sys_netcfg(&c, NN_NET_GET);
        printf("%s=%s\n", s->name, (c.services & s->key) ? "on" : "off");
        return;
    }
    int v = sys_sysctl(s->key, -1);
    if (s->kind == KIND_CONSOLE) printf("%s=%s\n", s->name, v >= 0 && v <= 2 ? console_modes[v] : "?");
    else if (s->kind == KIND_FONT) printf("%s=%s\n", s->name, v >= 0 && v <= 1 ? fonts[v] : "?");
    else printf("%s=%d\n", s->name, v);
}

static int set(const struct setting *s, const char *value)
{
    if (s->kind == KIND_SERVICE) {
        int on = !strcmp(value, "on") ? 1 : !strcmp(value, "off") ? 0 : -1;
        if (on < 0) return SYS_EINVAL;
        struct nn_netcfg c;
        sys_netcfg(&c, NN_NET_GET);
        c.services = (uint8_t)(on ? c.services | s->key : c.services & ~s->key);
        return sys_netcfg(&c, NN_NET_SERVICES) ? SYS_EINVAL : 0;
    }
    int v = 0;
    if (s->kind == KIND_CONSOLE) {
        v = -1;
        for (int i = 0; i < 3; ++i) if (!strcmp(value, console_modes[i])) v = i;
        if (v < 0) return SYS_EINVAL;
    } else if (s->kind == KIND_FONT) {
        v = !strcmp(value, "normal") ? 0 : !strcmp(value, "small") ? 1 : -1;
        if (v < 0) return SYS_EINVAL;
    } else {
        if (!*value) return SYS_EINVAL;
        for (const char *p = value; *p; ++p) {
            if (*p < '0' || *p > '9' || v > 99999999) return SYS_EINVAL;
            v = v * 10 + (*p - '0');
        }
    }
    int e = sys_sysctl(s->key, v);
    return e < 0 ? e : 0;
}

int cmd_sysctl(int argc, char **argv)
{
    if (argc == 1) {
        for (unsigned i = 0; i < COUNT; ++i) show(&settings[i]);
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "-h")) {
        for (unsigned i = 0; i < COUNT; ++i) printf("%s: %s\n", settings[i].name, settings[i].help);
        return 0;
    }
    int rc = 0;
    for (int i = 1; i < argc; ++i) {
        char *eq = strchr(argv[i], '=');
        size_t n = eq ? (size_t)(eq - argv[i]) : strlen(argv[i]);
        const struct setting *s = 0;
        for (unsigned k = 0; k < COUNT; ++k)
            if (strlen(settings[k].name) == n && !strncmp(settings[k].name, argv[i], n)) s = &settings[k];
        if (!s) { printf("sysctl: unknown setting '%s' (sysctl -h lists them)\n", argv[i]); rc = 1; continue; }
        int e = eq ? set(s, eq + 1) : 0;
        if (e == SYS_EINVAL) { printf("sysctl: invalid value for %s: '%s'\n", s->name, eq + 1); rc = 1; continue; }
        if (e) { printf("sysctl: %s: %s\n", s->name, errstr(e)); rc = 1; }
        show(s);
    }
    return rc;
}
