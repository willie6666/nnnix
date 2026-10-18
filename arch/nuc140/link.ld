/* SPDX-License-Identifier: GPL-3.0-or-later */
/* NUC140VE3CN: 128 KiB APROM at 0, 16 KiB SRAM at 0x20000000.
 * RAM layout: .data .bss | heap (kmalloc) | .noinit (crash record, fixed) | main stack
 * The boot code runs on a temporary stack at the top of the heap: kmain only
 * allocates from the bottom, and once the scheduler starts, the main stack
 * serves interrupts and exceptions only (measured peak 124 B), so it can be
 * small while the boot (about 600 B deep) costs no RAM afterwards. */
ENTRY(Reset_Handler)
MEMORY {
    FLASH (rx) : ORIGIN = 0x00000000, LENGTH = 128K
    RAM (rwx)  : ORIGIN = 0x20000000, LENGTH = 16K
}
__stack_top   = ORIGIN(RAM) + LENGTH(RAM);
__stack_limit = __stack_top - 512;    /* MSP: interrupts and exceptions */
__noinit_base = __stack_limit - 104;   /* the crash record */
__boot_stack_top = __noinit_base;     /* = __heap_end */

SECTIONS {
    .vectors : { KEEP(*(.vectors)) } > FLASH
    .text : {
        *(.text*) *(.rodata*)
        . = ALIGN(4); __isp_run = .; KEEP(*(.isp_run)) . = ALIGN(4); __isp_run_end = .;   /* arch/nuc140/fmc.c */
    } > FLASH
    .data : { __data_start = .; *(.data*) . = ALIGN(4); __data_end = .; } > RAM AT > FLASH
    __data_load = LOADADDR(.data);
    __flash_end = LOADADDR(.data) + SIZEOF(.data);   /* APROM above it holds program text (kernel/xip.c) */
    .bss (NOLOAD) : { __bss_start = .; *(.bss*) *(COMMON) . = ALIGN(4); __bss_end = .; } > RAM
    .noinit __noinit_base (NOLOAD) : { *(.noinit*) . = ALIGN(4); __noinit_end = .; } > RAM
    __heap_start = ALIGN(__bss_end, 8);
    __heap_end = __noinit_base;
    /DISCARD/ : { *(.comment) *(.ARM.exidx*) *(.ARM.extab*) }
}
ASSERT(__isp_run_end - __isp_run <= 48, "isp_run is larger than its copy buffer (ISP_RUN_WORDS)")
ASSERT(__bss_end <= __noinit_base, "static RAM overlaps .noinit")
ASSERT(__noinit_end <= __stack_limit, ".noinit overlaps the stack")
