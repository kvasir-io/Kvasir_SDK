/* a RAM image (common_ram_only.ld's shape): everything at its RAM address, nothing in flash */
MEMORY
{
    ram (rwx) : ORIGIN = 0x20000000, LENGTH = 256K
}
ENTRY(entry)
SECTIONS
{
    .vectors : { KEEP(*(.vectors)) } > ram
    .text : { . = ALIGN(16); KEEP(*(.text.entry)) *(.text*) *(.rodata*) . = ALIGN(4); } > ram
    .data : { *(.data*) . = ALIGN(4); } > ram
    .trap_data : { KEEP(*(.trap_data)) } > ram
    .noload_buffer (NOLOAD) : { *(.noload_buffer) } > ram
    .bss (NOLOAD) : { *(.bss*) } > ram
    /DISCARD/ : { *(.eeprom) *(.ARM.exidx*) *(.ARM.attributes) *(.kvasir_meta) }
}
