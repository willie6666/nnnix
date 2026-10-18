/* SPDX-License-Identifier: GPL-3.0-or-later */
/* keys [N]: prints the events of /dev/input (keyboard, mouse, the board's 1-9
 * keypad) until Esc, or until N events.
 * keys spin: holds /dev/input and runs without system calls until the board's
 * SW_INT button ends it (the test of that button). */
#include "nnnix.h"

int main(int argc, char **argv)
{
    int fd = sys_open("/dev/input", O_RDONLY);
    if (fd < 0) { printf("keys: /dev/input: %s\n", errstr(fd)); return 1; }
    if (argc > 1 && !strcmp(argv[1], "spin")) {
        printf("keys: spinning; SW_INT ends me\n");
        for (volatile uint32_t n = 0;; ++n) { }
    }
    int left = 0;
    for (const char *p = argc > 1 ? argv[1] : ""; *p >= '0' && *p <= '9'; ++p) left = left * 10 + (*p - '0');
    printf("keys: reading /dev/input (%s)\n", left ? "a number of events" : "Esc ends");
    struct nn_input_event ev[4];
    for (;;) {
        int n = sys_read(fd, ev, sizeof ev);
        if (n <= 0) break;
        for (int i = 0; i < n / (int)sizeof ev[0]; ++i) {
            const struct nn_input_event *e = &ev[i];
            if (e->type == NN_INPUT_KEY)
                printf("key 0x%02x %s%s%c%s\n", e->code, e->value ? "down" : "up", e->ch > ' ' ? " '" : "",
                       e->ch > ' ' ? e->ch : ' ', e->ch > ' ' ? "'" : "");
            else if (e->type == NN_INPUT_PAD)
                printf("pad %u %s\n", e->code, e->value ? "down" : "up");
            else if (e->type == NN_INPUT_MOUSE)
                printf("mouse buttons %u dx %d dy %d wheel %d\n", e->code, e->dx, e->dy, (int8_t)e->value);
            if ((e->type == NN_INPUT_KEY && e->code == NN_KEY_ESC && e->value) || (left && !--left)) {
                sys_close(fd);
                return 0;
            }
        }
    }
    sys_close(fd);
    return 0;
}
