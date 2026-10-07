/* the smallest RAM image with the shape linker/common_ram_only.ld gives one: the vectors with the RP2350's picobin
   block behind them (.after_vectors, bounded like linker/common_vectors_body.inc.ld), a NOLOAD gap, .text with
   .rodata, the descriptor, the init array and the RAM functions, then .data - everything in SRAM, nothing copied.
   RAM_FLASH_SEGMENT adds a section that loads into flash. */
MEMORY
{
    flash (rx)  : ORIGIN = 0x10000000, LENGTH = 1M
    ram   (rwx) : ORIGIN = 0x20000000, LENGTH = 64K
}
ENTRY(entry)
SECTIONS
{
    .vectors : {
        KEEP(*(.core_vectors))
        _LINKER_INTERN_after_vectors_start_ = .;
        KEEP(*(.after_vectors))
        _LINKER_INTERN_after_vectors_end_ = .;
    } > ram
    .noInitLowRam (NOLOAD) : { . += 64; } > ram
    .text : {
        KEEP(*(.text.entry)) *(.text*)
        *(.rodata*)
        . = ALIGN(4);
        KEEP(*(.kvasir_image_crc))
        . = ALIGN(4);
        KEEP(*(.init_array*))
        . = ALIGN(4);
        *(.ramfunc*)
    } > ram
    .data : { *(.data*) } > ram
    .bss (NOLOAD) : { *(.bss*) *(COMMON) } > ram
    .inflash : { KEEP(*(.inflash)) } > flash
    /DISCARD/ : { *(.ARM.exidx*) }
}
