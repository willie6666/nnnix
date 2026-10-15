/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NNNIX_HIDKEY_H
#define NNNIX_HIDKEY_H
#include <stdint.h>

#define HID_MOD_SHIFT 0x22u          /* left or right shift */

/* ASCII for a USB HID usage on a US layout, or 0 if the key has none. */
char hid_to_ascii(uint8_t usage, uint8_t modifiers, int caps_lock);
/* Short name for logging: "a", "Enter", "F1", or NULL when unnamed. */
const char *hid_key_name(uint8_t usage);

#endif
