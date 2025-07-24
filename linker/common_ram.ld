SECTIONS {
    .noInitLowRam /*(NOLOAD)*/ : {
        . = ALIGN(4);
        _LINKER_INTERN_noInitLowRam_start_ = .;
        . = ALIGN(4);
        *(.noInitLowRam*)
        . = ALIGN(4);
        _LINKER_INTERN_noInitLowRam_end_ = .;
        . = ALIGN(4);
    } > ram

    .stack /*(NOLOAD)*/ : {
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

    _LINKER_INTERN_data_start_flash_ = ORIGIN(flash) + SIZEOF(.text);
    .data :  AT(_LINKER_INTERN_data_start_flash_) {
        FILL(0xFFDEFFDE); /* ARM "udf #255" instruction - triggers HardFault if PC goes to uninitialized memory */
        . = ALIGN(4);
        _LINKER_INTERN_data_start_ = .;
        . = ALIGN(4);
        *(.data*)
        . = ALIGN(4);
        _LINKER_INTERN_data_end_ = .;
        . = ALIGN(4);
    } > ram
    _LINKER_INTERN_data_end_flash_ = _LINKER_INTERN_data_start_flash_ + SIZEOF(.data);
    .bss (NOLOAD) : AT(.) {
        . = ALIGN(4);
        _LINKER_INTERN_bss_start_ = .;
        . = ALIGN(4);
        *(.bss*)
        . = ALIGN(4);
        _LINKER_INTERN_bss_end_ = .;
        . = ALIGN(4);
    } > ram

    .noInit /*(NOLOAD)*/ : {
        . = ALIGN(4);
        _LINKER_INTERN_noInit_start_ = .;
        . = ALIGN(4);
        *(.noInit*)
        . = ALIGN(4);
        _LINKER_INTERN_noInit_end_ = .;
        . = ALIGN(4);
    } > ram

    .heap (NOLOAD) : {
    . = ALIGN(4);
    _LINKER_INTERN_heap_start_ = .;
    . = ALIGN(4);
    . = . + cmake_heap_size;
    . = ALIGN(4);
    _LINKER_INTERN_heap_end_ = .;
    . = ALIGN(4);
    } > ram
}
