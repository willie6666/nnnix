/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "kbd.h"
#include "hidkey.h"

#define MOD_CTRL 0x11u
#define MOD_SHIFT 0x22u
#define USAGE_CAPS 0x39u

static int contains(const uint8_t *report, uint8_t usage)
{
    for (int i = 2; i < 8; ++i) if (report[i] == usage) return 1;
    return 0;
}

static int sequence(const char *s, uint8_t *out, int max)
{
    int n = 0;
    while (s[n] && n < max) { out[n] = (uint8_t)s[n]; ++n; }
    return n;
}

/* Bytes for one key; 0 for keys without a terminal meaning. */
static int translate(uint8_t usage, uint8_t mods, int caps, uint8_t *out, int max)
{
    if (max < 4) return 0;
    switch (usage) {
    case 0x28: out[0] = '\r'; return 1;
    case 0x2a: out[0] = 0x7f; return 1;
    case 0x4b: out[0] = KBD_PAGE_UP; return 1;
    case 0x4e: out[0] = KBD_PAGE_DOWN; return 1;
    case 0x4f: return sequence("\033[C", out, max);
    case 0x50: return sequence("\033[D", out, max);
    case 0x51: if (mods & MOD_SHIFT) { out[0] = KBD_LINE_DOWN; return 1; } return sequence("\033[B", out, max);
    case 0x52: if (mods & MOD_SHIFT) { out[0] = KBD_LINE_UP; return 1; } return sequence("\033[A", out, max);
    case 0x4a: return sequence("\033[H", out, max);
    case 0x4d: return sequence("\033[F", out, max);
    case 0x4c: return sequence("\033[3~", out, max);
    default: break;
    }
    if (mods & MOD_CTRL) {                        /* text size: Ctrl with - = 0, or the keypad's - + */
        if (usage == 0x2d || usage == 0x56) { out[0] = KBD_TEXT_SMALL; return 1; }
        if (usage == 0x2e || usage == 0x27 || usage == 0x57) { out[0] = KBD_TEXT_LARGE; return 1; }
    }
    char c = hid_to_ascii(usage, mods, caps);
    if (!c) return 0;
    if (mods & MOD_CTRL) {
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 1);
        else if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 1);
        else return 0;
    }
    out[0] = (uint8_t)c;
    return 1;
}

int kbd_report(struct kbd *k, const uint8_t report[8], uint32_t now, uint8_t *out, int max)
{
    int n = 0;
    for (int i = 2; i < 8; ++i) {
        uint8_t u = report[i];
        if (u <= 1 || contains(k->prev, u)) continue;   /* 1 = rollover error */
        if (u == USAGE_CAPS) { k->caps ^= 1u; continue; }
        int m = translate(u, report[0], k->caps, out + n, max - n);
        if (m) {
            n += m;
            k->held = u;
            k->held_mods = report[0];
            k->repeat_at = now + KBD_REPEAT_DELAY;
        }
    }
    if (k->held && !contains(report, k->held)) k->held = 0;
    if (k->held) k->held_mods = report[0];
    for (int i = 0; i < 8; ++i) k->prev[i] = report[i];
    return n;
}

int kbd_repeat(struct kbd *k, uint32_t now, uint8_t *out, int max)
{
    if (!k->held || (int32_t)(now - k->repeat_at) < 0) return 0;
    k->repeat_at = now + KBD_REPEAT_RATE;
    return translate(k->held, k->held_mods, k->caps, out, max);
}
