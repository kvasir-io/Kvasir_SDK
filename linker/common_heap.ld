/* Heap section for flash-based configuration (placed after .noInit) */

SECTIONS {
    /* Heap grows upward */
    .heap (NOLOAD) : AT(.) {
        . = ALIGN(4);
        _LINKER_INTERN_heap_start_ = .;
        . = ALIGN(4);
        . = . + cmake_heap_size;
        . = ALIGN(4);
        _LINKER_INTERN_heap_end_ = .;
        . = ALIGN(4);
    } > ram
}
