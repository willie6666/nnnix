/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The bootloader: LDROM, mapped at 0 when the chip boots from it. */
ENTRY(boot_reset)
MEMORY {
    LDROM (rx) : ORIGIN = 0x00000000, LENGTH = 4K
    RAM (rwx)  : ORIGIN = 0x20000000, LENGTH = 16K
}
SECTIONS {
    .vectors : { KEEP(*(.vectors)) } > LDROM
    .text : { *(.text*) *(.rodata*) . = ALIGN(4); } > LDROM
    .data : { *(.data*) } > RAM AT > LDROM
    ASSERT(SIZEOF(.data) == 0, "the bootloader has no initialised data")
    .bss (NOLOAD) : { __bss_start = .; *(.bss*) *(COMMON) . = ALIGN(4); __bss_end = .; } > RAM
    /DISCARD/ : { *(.comment) *(.ARM.exidx*) *(.ARM.extab*) }
}
