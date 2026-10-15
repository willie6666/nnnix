/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "hidkey.h"

/* Usages 0x04..0x38, US layout: unshifted and shifted. */
static const char plain[]   = "abcdefghijklmnopqrstuvwxyz1234567890\n\x1b\b\t -=[]\\#;'`,./";
static const char shifted[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()\n\x1b\b\t _+{}|~:\"~<>?";

char hid_to_ascii(uint8_t usage, uint8_t modifiers, int caps_lock)
{
    if (usage < 0x04 || usage > 0x38) return 0;
    int shift = (modifiers & HID_MOD_SHIFT) != 0;
    if (caps_lock && usage <= 0x1d) shift = !shift;       /* letters only */
    return (shift ? shifted : plain)[usage - 0x04];
}

const char *hid_key_name(uint8_t usage)
{
    static const char *const named[] = {
        [0x28] = "Enter", [0x29] = "Esc", [0x2a] = "Backspace", [0x2b] = "Tab",
        [0x2c] = "Space", [0x39] = "CapsLock",
        [0x3a] = "F1", [0x3b] = "F2", [0x3c] = "F3", [0x3d] = "F4", [0x3e] = "F5", [0x3f] = "F6",
        [0x40] = "F7", [0x41] = "F8", [0x42] = "F9", [0x43] = "F10", [0x44] = "F11", [0x45] = "F12",
        [0x4a] = "Home", [0x4b] = "PageUp", [0x4c] = "Delete", [0x4d] = "End", [0x4e] = "PageDown",
        [0x4f] = "Right", [0x50] = "Left", [0x51] = "Down", [0x52] = "Up",
    };
    static char single[2];
    if (usage < sizeof named / sizeof named[0] && named[usage]) return named[usage];
    char c = hid_to_ascii(usage, 0, 0);
    if (c > ' ' && c < 0x7f) { single[0] = c; single[1] = 0; return single; }
    return 0;
}
