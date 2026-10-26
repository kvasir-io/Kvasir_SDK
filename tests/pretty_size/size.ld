/* tests/elf_image/flash.ld with literal MEMORY values (linker_utils' MEMORY regex takes no expressions): .data and a
   second initialised RAM section loaded after .text, an EEPROM region, a NOLOAD buffer and a non-ALLOC section */
MEMORY
{
    flash  (rx)  : ORIGIN = 0x10000000, LENGTH = 1M
    eeprom (rw)  : ORIGIN = 0x10100000, LENGTH = 4K
    ram    (rwx) : ORIGIN = 0x20000000, LENGTH = 64K
}
ENTRY(entry)
SECTIONS
{
    .vectors : { KEEP(*(.vectors)) } > flash
    .text : { . = ALIGN(16); KEEP(*(.text.entry)) *(.text*) *(.rodata*) . = ALIGN(4); } > flash
    .data : { *(.data*) . = ALIGN(4); } > ram AT > flash
    .trap_data : { KEEP(*(.trap_data)) } > ram AT > flash
    .eeprom : { KEEP(*(.eeprom)) } > eeprom
    .noload_buffer (NOLOAD) : { *(.noload_buffer) } > ram
    .bss (NOLOAD) : { *(.bss*) } > ram
    .kvasir_meta 0 (INFO) : { KEEP(*(.kvasir_meta)) }
    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.attributes) }
}
