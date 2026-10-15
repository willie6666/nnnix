/* Minimal host test helpers. */
#ifndef NNNIX_CHECK_H
#define NNNIX_CHECK_H
#include <stdio.h>

static int check_failures, check_count;

#define CHECK(cond) do { \
    check_count++; \
    if (!(cond)) { check_failures++; fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

#define CHECK_EQ_INT(a, b) do { \
    long long va_ = (long long)(a), vb_ = (long long)(b); \
    check_count++; \
    if (va_ != vb_) { check_failures++; fprintf(stderr, "%s:%d: %s == %lld, expected %lld\n", __FILE__, __LINE__, #a, va_, vb_); } \
} while (0)

static int check_summary(const char *name)
{
    printf("%s: %d checks, %d failed\n", name, check_count, check_failures);
    return check_failures ? 1 : 0;
}

#endif
