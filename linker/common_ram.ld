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

/* The stack fills what RAM is left (common_stack_body.inc.ld): the sizes of every other section in region ram.
   A chip whose script puts a section of its own into ram (chip_rp2040's RAM-only .boot2) sets
   _LINKER_INTERN_chip_ram_other_size_ BEFORE it includes this file. */
PROVIDE(_LINKER_INTERN_chip_ram_other_size_ = 0);
_LINKER_INTERN_ram_other_size_ = SIZEOF(.noInitLowRam) + SIZEOF(.stack1) + SIZEOF(.data) + SIZEOF(.bss) + SIZEOF(.noInit)
                               + _LINKER_INTERN_chip_ram_other_size_;
