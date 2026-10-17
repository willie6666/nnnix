/* SPDX-License-Identifier: GPL-3.0-or-later */
/* audioshare: two tasks write to one open /dev/audio at once (the worker gets
 * it as its standard output): small writes here, large ones in the worker,
 * 8000 samples each. The samples are 0, which keeps the buzzer off. The
 * kernel logs the samples played when the device closes: 16000 if no write
 * was lost or doubled. */
#include "nnnix.h"

#define EACH 8000u

static uint8_t big[1000], small[16];             /* zeros */

static int worker(int argc, char **argv)
{
    (void)argc; (void)argv;
    for (uint32_t n = 0; n < EACH; n += sizeof big)
        if (sys_write(1, big, sizeof big) != (int)sizeof big) return 1;
    return 0;
}

int main(void)
{
    int fd = sys_open("/dev/audio", O_WRONLY);
    if (fd < 0) { printf("audioshare: /dev/audio: %s\n", errstr(fd)); return 1; }
    char *name[] = { "worker", 0 };
    struct nn_spawn s = { worker, name, 1, -1, fd, 768 };
    int pid = sys_spawn(&s);
    if (pid < 0) { printf("audioshare: %s\n", errstr(pid)); return 1; }
    int bad = 0;
    for (uint32_t n = 0; n < EACH; n += sizeof small)
        if (sys_write(fd, small, sizeof small) != (int)sizeof small) bad = 1;
    int status = 1;
    sys_wait(pid, &status, NN_FOREVER);
    sys_close(fd);                                   /* the worker's copy is closed already */
    printf("audioshare: %s\n", bad || status ? "a write failed" : "16000 samples written");
    return bad || status;
}
