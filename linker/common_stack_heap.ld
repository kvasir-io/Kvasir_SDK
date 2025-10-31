/* Stack and heap for RAM-only configuration (placed after .noInit) */

SECTIONS {
    /* Stack with protection */
    .stack (NOLOAD) : AT(.) {
        . = ALIGN(8);
        _LINKER_INTERN_stack_start_ = .;
        . = ALIGN(8);
        *(.stackProtector*)
        _LINKER_INTERN_stackProtector_end_ = .;
        . = . + ((cmake_min_stack_size + cmake_stack_size_extra) - ( cmake_stack_size_extra == 0 ? 0 : ((_LINKER_INTERN_stackProtector_end_ - _LINKER_INTERN_stack_start_))));
        . = ALIGN(8);
        _LINKER_INTERN_stack_end_ = .;
        . = ALIGN(8);
    } > ram

    /* Heap grows upward from end of stack */
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
