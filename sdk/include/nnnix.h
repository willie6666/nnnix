/* SPDX-License-Identifier: GPL-3.0-or-later */
/* nnnix programs: system calls (sys.h) and a small C library. The library
 * functions live in the firmware's flash and are reached through the table
 * that _start receives, so a program's RAM holds only its own code and data. */
#ifndef NNNIX_SDK_H
#define NNNIX_SDK_H
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include "sys.h"
#include "nnlib.h"

extern const struct nn_lib *__nn_lib;

int   printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int   vprintf(const char *fmt, va_list ap);
int   snprintf(char *buf, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int   vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
int   puts(const char *s);           /* s and a newline */
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int   memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int   strcmp(const char *a, const char *b);
int   strncmp(const char *a, const char *b, size_t n);
char *strcpy(char *dst, const char *src);
char *strchr(const char *s, int c);
const char *errstr(int error);       /* text for a negative SYS_E* result */
int   nn_write_all(int fd, const void *p, size_t n);   /* all of it, again after short writes: n or an error */
int   fputs_fd(const char *s, int fd);
int   nn_interrupted(void);          /* Ctrl-C typed on the terminal since the last check */
void *malloc(size_t n);              /* heap blocks are freed when the program ends */
void  free(void *p);
__attribute__((noreturn)) void exit(int code);

#endif
