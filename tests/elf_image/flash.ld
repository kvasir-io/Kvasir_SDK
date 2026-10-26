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
/* the image end and the flash end as linker/common.ld and common_flash.ld give them; a test moves the end with
   --defsym=_LINKER_INTERN_image_end_flash_=..., as a chip script would */
_LINKER_INTERN_rom_end_ = ORIGIN(flash) + LENGTH(flash);
PROVIDE(_LINKER_INTERN_image_end_flash_ = LOADADDR(.trap_data) + SIZEOF(.trap_data));
_LINKER_image_end_flash_ = _LINKER_INTERN_image_end_flash_;
