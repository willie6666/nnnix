/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NNNIX_GAME_H
#define NNNIX_GAME_H
#include <stdint.h>

/* What the games in bin/ share (bin/game.c): the LCD as a 128x64 picture
 * drawn in RAM and shown whole, text in the firmware's fonts, the keys as
 * actions, a random number, and the time.
 *
 * Keys: arrows or WASD and the keypad's 2 4 6 8 are the directions; Space,
 * Enter and keypad 5 are the action key (GAME_A: not the letter A, which is
 * left); Esc, q and keypad 1 end the game (the board's SW_INT button always
 * does). */
#define GAME_W 128
#define GAME_H 64
enum { GAME_LEFT = 1, GAME_RIGHT = 2, GAME_UP = 4, GAME_DOWN = 8, GAME_A = 16, GAME_QUIT = 32 };

int  game_open(const char *name);    /* /dev/input and /dev/lcd: 0, or 1 (said why) */
void game_close(void);

/* Input: game_poll reads what has come; pressed returns the actions pressed
 * since it was last called (and forgets them), held those held down now. */
void game_poll(void);
unsigned game_pressed(void);
unsigned game_held(void);
/* Show the picture, then wait until `at` (sys_uptime ms) reading the keys;
 * or only wait. */
void game_show_until(uint32_t at);
void game_wait_until(uint32_t at);

void game_clear(void);
void game_pixel(int x, int y, int on);
int  game_get(int x, int y);
void game_fill(int x, int y, int w, int h, int on);
void game_box(int x, int y, int w, int h);              /* an outline */
/* Text: small 3x5 (a character every 4 pixels) or big 5x7 (every 6); x < 0
 * centres it. The width it took. */
int  game_text(int x, int y, const char *s);
int  game_big(int x, int y, const char *s);
int  game_number(int x, int y, unsigned n);              /* small digits */

uint32_t game_rand(void);
/* Game over: shows the score and the best of this run over the picture, and
 * waits for quit (0) or any other key (1: play again). */
int  game_over(unsigned score, unsigned *best);

#endif
