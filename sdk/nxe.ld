/* SPDX-License-Identifier: GPL-3.0-or-later */
/* NXE programs (see tools/mknxe.py): text at 0, data and bss at 0x10000000 in
 * a segment of their own, so that r9-relative (SBREL) offsets count from the
 * data start. The loader puts text in APROM (or RAM) and data in RAM. */
ENTRY(_start)
PHDRS { text PT_LOAD; data PT_LOAD; }
SECTIONS {
    . = 0;
    .text : { KEEP(*(.text._start)) *(.text*) *(.rodata*) . = ALIGN(4); } :text
    . = 0x10000000;
    .data : { *(.data*) . = ALIGN(4); } :data
    .bss (NOLOAD) : { *(.bss*) *(COMMON) . = ALIGN(4); } :data
    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.comment) *(.ARM.attributes) }
}
