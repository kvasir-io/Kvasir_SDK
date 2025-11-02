/* Flash-based execution: RAM sections with data copied from flash */

SECTIONS {
    .noInitLowRam (NOLOAD) : AT(.) {
        INCLUDE common_noInitLowRam_body.inc.ld
    } > ram

    .stack (NOLOAD) : AT(.) {
        INCLUDE common_stack_body.inc.ld
    } > ram

    .data : AT(_LINKER_INTERN_data_start_flash_) {
        INCLUDE common_data_body.inc.ld
    } > ram
    _LINKER_INTERN_data_end_flash_ = _LINKER_INTERN_data_start_flash_ + SIZEOF(.data);

    .bss (NOLOAD) : AT(.) {
        INCLUDE common_bss_body.inc.ld
    } > ram

    .noInit (NOLOAD) : AT(.) {
        INCLUDE common_noInit_body.inc.ld
    } > ram

    .heap (NOLOAD) : AT(.) {
        INCLUDE common_heap_body.inc.ld
    } > ram
}
