/* an RP-like flash image: .data and a second initialised RAM section loaded after .text, an EEPROM region,
   a NOLOAD buffer and a non-ALLOC section */
MEMORY
{
    flash  (rx)  : ORIGIN = FLASH_ORIGIN, LENGTH = 1M
    eeprom (rw)  : ORIGIN = FLASH_ORIGIN + 0x100000, LENGTH = 4K
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
