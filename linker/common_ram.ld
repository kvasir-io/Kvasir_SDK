/* Flash-based execution: RAM sections with data copied from flash */

SECTIONS {
    .noInitLowRam (NOLOAD) : AT(ADDR(.noInitLowRam)) {
        INCLUDE common_noInitLowRam_body.inc.ld
    } > ram

    .stack (NOLOAD) : AT(ADDR(.stack)) {
        INCLUDE common_stack_body.inc.ld
    } > ram

    .stack1 (NOLOAD) : AT(ADDR(.stack1)) {
        INCLUDE common_stack1_body.inc.ld
    } > ram

    .data : AT(_LINKER_INTERN_data_start_flash_) {
        INCLUDE common_data_body.inc.ld
    } > ram
    _LINKER_INTERN_data_end_flash_ = _LINKER_INTERN_data_start_flash_ + SIZEOF(.data);

    .bss (NOLOAD) : AT(ADDR(.bss)) {
        INCLUDE common_bss_body.inc.ld
    } > ram

    .noInit (NOLOAD) : AT(ADDR(.noInit)) {
        INCLUDE common_noInit_body.inc.ld
    } > ram

    .heap (NOLOAD) : AT(ADDR(.heap)) {
        INCLUDE common_heap_body.inc.ld
    } > ram
}
