/* RAM-only execution: all sections in RAM, no flash copy needed */

SECTIONS {
    . = ORIGIN(ram);

    .vectors : {
        INCLUDE common_vectors_body.inc.ld
    } > ram

    .noInitLowRam (NOLOAD) : AT(.) {
        INCLUDE common_noInitLowRam_body.inc.ld
    } > ram

    .text : {
        INCLUDE common_text_body.inc.ld
    } > ram

    .data : {
        INCLUDE common_data_body.inc.ld
    } > ram

    .bss (NOLOAD) : AT(.) {
        INCLUDE common_bss_body.inc.ld
    } > ram

    .noInit (NOLOAD) : AT(.) {
        INCLUDE common_noInit_body.inc.ld
    } > ram

    .stack (NOLOAD) : AT(.) {
        INCLUDE common_stack_body.inc.ld
    } > ram

    .heap (NOLOAD) : AT(.) {
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
