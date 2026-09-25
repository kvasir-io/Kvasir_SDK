/* the smallest image with Kvasir's regions: flash, RAM, the RAM functions' marks */
MEMORY
{
    flash (rx)  : ORIGIN = 0x10000000, LENGTH = 1M
    ram   (rwx) : ORIGIN = 0x20000000, LENGTH = 64K
}
ENTRY(entry)
SECTIONS
{
    .text : { KEEP(*(.text.main)) *(.text*) *(.rodata*) } > flash
    .data : { *(.data*) } > ram AT > flash
    .bss (NOLOAD) : { *(.bss*) *(COMMON) } > ram
    kvasir_ram_funcs 0 (INFO) : { KEEP(*(kvasir_ram_funcs)) }
    kvasir_ram_funcs_calls_flash 0 (INFO) : { KEEP(*(kvasir_ram_funcs_calls_flash)) }
    /DISCARD/ : { *(.ARM.exidx*) }
}
_LINKER_INTERN_rom_start_ = ORIGIN(flash);
_LINKER_INTERN_rom_end_   = ORIGIN(flash) + LENGTH(flash);
_LINKER_INTERN_ram_start_ = ORIGIN(ram);
_LINKER_INTERN_ram_end_   = ORIGIN(ram) + LENGTH(ram);
