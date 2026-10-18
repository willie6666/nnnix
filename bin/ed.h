/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NNNIX_ED_H
#define NNNIX_ED_H
#include <stdint.h>

/* The core of ed, a line editor in the style of UNIX ed(1), small enough for a
 * 21x8 screen. The text is one heap block with '\n' after every line.
 *
 * Addresses: N . $ +N -N /text/ (literal, searched forward with wrap-around),
 * A,B and , (1,$) and ; (.,$).
 * Commands: (address only) print; p n z; a i c (input ends with a lone '.');
 * d j s/old/new/[g] (literal); = ; w [file] wq q Q e file r [file] f [file].
 * Errors print "? " and a reason. q, e and end of input warn once about
 * unsaved changes. */
#define ED_QUIT 1
#define ED_PATH 64
#define ED_PAGE 7                    /* lines shown by z: a screen minus the prompt */

struct ed {
    char *buf;
    uint32_t len, cap;
    int32_t lines, cur;
    int32_t input_at;                /* in input mode: new lines go after this line; else -1 */
    uint8_t dirty, warned, silent;
    char file[ED_PATH];
};

void ed_init(struct ed *e, int silent);
void ed_free_buffer(struct ed *e);
int  ed_load(struct ed *e, const char *path, int32_t after);   /* bytes read, or a negative error */
int  ed_line(struct ed *e, const char *line);                  /* one line without '\n'; ED_QUIT to leave */

/* Provided by the program (the shell command, or the host test). */
void ed_out(const char *s, uint32_t n);
void *ed_alloc(uint32_t n);
void ed_release(void *p);
int  ed_grow(void *p, uint32_t n);    /* in place: 0, or -1 */
int  ed_open(const char *path, int write);                     /* fd, or a negative error */
int  ed_read(int fd, void *buf, uint32_t n);
int  ed_write(int fd, const void *buf, uint32_t n);
int  ed_close(int fd);

#endif
