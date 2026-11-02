/* The stack bound test's image: flash, RAM, the vector table first; .kvasir.isr_levels as Kvasir_SDK linker/common.ld
   places it (INFO: not loaded). */
MEMORY { flash (rx) : ORIGIN = 0x10000000, LENGTH = 64K  ram (rwx) : ORIGIN = 0x20000000, LENGTH = 16K }
SECTIONS {
    .vectors : { KEEP(*(.vectors)) } > flash
    .text : { *(.text*) *(.rodata*) } > flash
    .bss (NOLOAD) : { *(.bss*) *(COMMON) } > ram
    .kvasir.isr_levels 0 (INFO) : { KEEP(*(.kvasir.isr_levels)) }
    stack_top = ORIGIN(ram) + LENGTH(ram);
}
