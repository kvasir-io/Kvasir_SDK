/* RAM-only execution: all sections in RAM, no flash copy needed */

SECTIONS {
    . = ORIGIN(ram);

    .vectors : {
        INCLUDE common_vectors_body.inc.ld
    } > ram

    .noInitLowRam (NOLOAD) : AT(ADDR(.noInitLowRam)) {
        INCLUDE common_noInitLowRam_body.inc.ld
    } > ram

    .text : {
        INCLUDE common_text_body.inc.ld
    } > ram

    .data : {
        INCLUDE common_data_body.inc.ld
    } > ram

    .bss (NOLOAD) : AT(ADDR(.bss)) {
        INCLUDE common_bss_body.inc.ld
    } > ram

    .noInit (NOLOAD) : AT(ADDR(.noInit)) {
        INCLUDE common_noInit_body.inc.ld
    } > ram

    .stack (NOLOAD) : AT(ADDR(.stack)) {
        INCLUDE common_stack_body.inc.ld
    } > ram

    .stack1 (NOLOAD) : AT(ADDR(.stack1)) {
        INCLUDE common_stack1_body.inc.ld
    } > ram

    .heap (NOLOAD) : AT(ADDR(.heap)) {
        INCLUDE common_heap_body.inc.ld
    } > ram
}

/* Flash symbols set to zero for RAM-only (kept for compatibility) */
_LINKER_INTERN_data_start_flash_ = 0;
_LINKER_INTERN_data_end_flash_ = 0;

/* No flash memory in RAM-only configuration */
_LINKER_INTERN_rom_start_   = 0;
_LINKER_INTERN_rom_size_    = 0;
_LINKER_INTERN_rom_end_     = 0;

/* The stack fills what RAM is left (common_stack_body.inc.ld): the sizes of every other section in region ram.
   A chip whose script puts a section of its own into ram (chip_rp2040's RAM-only .boot2) sets
   _LINKER_INTERN_chip_ram_other_size_ BEFORE it includes this file. */
PROVIDE(_LINKER_INTERN_chip_ram_other_size_ = 0);
_LINKER_INTERN_ram_other_size_ = SIZEOF(.vectors) + SIZEOF(.noInitLowRam) + SIZEOF(.text) + SIZEOF(.data) + SIZEOF(.bss) + SIZEOF(.noInit) + SIZEOF(.stack1)
                               + _LINKER_INTERN_chip_ram_other_size_;
