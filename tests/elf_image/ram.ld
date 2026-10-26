/* a RAM image (common_ram_only.ld's shape): everything at its RAM address, nothing in flash */
MEMORY
{
    ram (rwx) : ORIGIN = 0x20000000, LENGTH = 256K
}
ENTRY(entry)
SECTIONS
{
    .vectors : { KEEP(*(.vectors)) } > ram
    /* 8 bytes of .vectors, then 16-aligned: an 8-byte hole picotool plugs itself, which must stay inside its PT_LOAD */
    .text : ALIGN(16) { KEEP(*(.text.entry)) *(.text*) *(.rodata*) . = ALIGN(4); } > ram
    /* between two loaded sections, as common_ram_only.ld's .noInitLowRam and .ramExec: GNU ld leaves it as a hole
       inside one PT_LOAD, larger than the next section's alignment */
    .noload_buffer (NOLOAD) : AT(ADDR(.noload_buffer)) { KEEP(*(.noload_buffer)) } > ram
    .data : { *(.data*) . = ALIGN(4); } > ram
    .trap_data : { KEEP(*(.trap_data)) } > ram
    .bss (NOLOAD) : { *(.bss*) } > ram
    /DISCARD/ : { *(.eeprom) *(.ARM.exidx*) *(.ARM.attributes) *(.kvasir_meta) }
}
/* as common_ram_only.ld: nothing in flash */
_LINKER_INTERN_rom_end_ = 0;
_LINKER_image_end_flash_ = 0;
