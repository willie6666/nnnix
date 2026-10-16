/* SPDX-License-Identifier: GPL-3.0-or-later */
/* NXE programs are linked at 0 and moved by the loader (see tools/mknxe.py). */
ENTRY(_start)
SECTIONS {
    . = 0;
    .text : { KEEP(*(.text._start)) *(.text*) *(.rodata*) . = ALIGN(4); }
    .data : { *(.data*) . = ALIGN(4); }
    .bss (NOLOAD) : { *(.bss*) *(COMMON) . = ALIGN(4); }
    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.comment) *(.ARM.attributes) }
}
