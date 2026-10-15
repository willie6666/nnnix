/* NUC140VE3CN: 128 KiB APROM at 0, 16 KiB SRAM at 0x20000000.
 * RAM layout: .data .bss | free | .noinit (crash record, fixed) | main stack */
ENTRY(Reset_Handler)
MEMORY {
    FLASH (rx) : ORIGIN = 0x00000000, LENGTH = 128K
    RAM (rwx)  : ORIGIN = 0x20000000, LENGTH = 16K
}
__stack_top   = ORIGIN(RAM) + LENGTH(RAM);
__stack_limit = __stack_top - 2048;
__noinit_base = __stack_limit - 512;

SECTIONS {
    .vectors : { KEEP(*(.vectors)) } > FLASH
    .text : { *(.text*) *(.rodata*) . = ALIGN(4); } > FLASH
    .data : { __data_start = .; *(.data*) . = ALIGN(4); __data_end = .; } > RAM AT > FLASH
    __data_load = LOADADDR(.data);
    .bss (NOLOAD) : { __bss_start = .; *(.bss*) *(COMMON) . = ALIGN(4); __bss_end = .; } > RAM
    .noinit __noinit_base (NOLOAD) : { *(.noinit*) . = ALIGN(4); __noinit_end = .; } > RAM
    /DISCARD/ : { *(.comment) *(.ARM.exidx*) *(.ARM.extab*) }
}
ASSERT(__bss_end <= __noinit_base, "static RAM overlaps .noinit")
ASSERT(__noinit_end <= __stack_limit, ".noinit overlaps the stack")
