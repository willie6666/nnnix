#ifndef NNNIX_KERNEL_H
#define NNNIX_KERNEL_H
#include <stdint.h>

extern const char nnnix_version[];

uint32_t stack_used(void);            /* main stack high-water mark in bytes */
uint32_t stack_size(void);
uint32_t ram_static(void);            /* .data + .bss + .noinit bytes */

#endif
