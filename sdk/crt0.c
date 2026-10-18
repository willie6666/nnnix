/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Program entry and the C library, forwarded to the firmware's table. */
#include "nnnix.h"
#include "fmt.h"

const struct nn_lib *__nn_lib;
int main(int argc, char **argv);

/* The loader calls this with the arguments and the library table. Returning
 * from it (or from main) exits with that code. */
__attribute__((used, section(".text._start")))
int _start(int argc, char **argv, const struct nn_lib *lib)
{
    if (lib->magic != NN_LIB_MAGIC) return 126;
    __nn_lib = lib;
    return main(argc, argv);
}

int vprintf(const char *fmt, va_list ap) { return __nn_lib->vprintf(fmt, ap); }
int printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = __nn_lib->vprintf(fmt, ap);
    va_end(ap);
    return n;
}
int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap) { return __nn_lib->vsnprintf(buf, n, fmt, ap); }
int snprintf(char *buf, size_t n, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = __nn_lib->vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}
int nn_vsnprintf(char *buf, size_t n, const char *fmt, va_list ap) { return __nn_lib->vsnprintf(buf, n, fmt, ap); }
int nn_snprintf(char *buf, size_t n, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = __nn_lib->vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}
int puts(const char *s) { return printf("%s\n", s); }
int nn_write_all(int fd, const void *p, size_t n)
{
    const char *s = p;
    for (size_t done = 0; done < n;) {
        int w = sys_write(fd, s + done, n - done);
        if (w < 0) return w;
        if (w == 0) return SYS_EIO;
        done += (size_t)w;
    }
    return (int)n;
}
int fputs_fd(const char *s, int fd) { return nn_write_all(fd, s, strlen(s)); }
int nn_interrupted(void) { return sys_ttyintr() > 0; }
void *memcpy(void *d, const void *s, size_t n) { return __nn_lib->memcpy(d, s, n); }
void *memmove(void *d, const void *s, size_t n) { return __nn_lib->memmove(d, s, n); }
void *memset(void *d, int c, size_t n) { return __nn_lib->memset(d, c, n); }
int memcmp(const void *a, const void *b, size_t n) { return __nn_lib->memcmp(a, b, n); }
size_t strlen(const char *s) { return __nn_lib->strlen(s); }
int strcmp(const char *a, const char *b) { return __nn_lib->strcmp(a, b); }
int strncmp(const char *a, const char *b, size_t n) { return __nn_lib->strncmp(a, b, n); }
char *strcpy(char *d, const char *s) { return __nn_lib->strcpy(d, s); }
char *strchr(const char *s, int c) { return __nn_lib->strchr(s, c); }
const char *errstr(int e) { return __nn_lib->errstr(e); }
void *malloc(size_t n) { return sys_alloc(n); }
void free(void *p) { if (p) sys_free(p); }
void exit(int code) { sys_exit(code); }

/* Helpers the compiler calls: the Cortex-M0 has no divide instruction. */
uint32_t __aeabi_uidiv(uint32_t n, uint32_t d) { return __nn_lib->uidiv(n, d); }
uint64_t __aeabi_uidivmod(uint32_t n, uint32_t d) { return __nn_lib->uidivmod(n, d); }
int32_t __aeabi_idiv(int32_t n, int32_t d) { return __nn_lib->idiv(n, d); }
uint64_t __aeabi_idivmod(int32_t n, int32_t d) { return __nn_lib->idivmod(n, d); }
void __aeabi_memcpy(void *d, const void *s, size_t n) { __nn_lib->memcpy(d, s, n); }
void __aeabi_memcpy4(void *d, const void *s, size_t n) { __nn_lib->memcpy(d, s, n); }
void __aeabi_memcpy8(void *d, const void *s, size_t n) { __nn_lib->memcpy(d, s, n); }
void __aeabi_memmove(void *d, const void *s, size_t n) { __nn_lib->memmove(d, s, n); }
void __aeabi_memset(void *d, size_t n, int c) { __nn_lib->memset(d, c, n); }   /* note the order */
void __aeabi_memclr(void *d, size_t n) { __nn_lib->memset(d, 0, n); }
void __aeabi_memclr4(void *d, size_t n) { __nn_lib->memset(d, 0, n); }
void __aeabi_memclr8(void *d, size_t n) { __nn_lib->memset(d, 0, n); }
