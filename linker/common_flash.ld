SECTIONS {
    . = ORIGIN(flash);
    .text : {
        FILL(0xFFDEFFDE); /* ARM "udf #255" instruction - triggers HardFault if PC goes to uninitialized memory */
        . = ALIGN(4);
        KEEP(*(.core_vectors*))
        KEEP(*(.after_vectors*))
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
        _LINKER_INTERN_init_array_start_ = .;
        . = ALIGN(4);
        KEEP(*(.init_array*))
        . = ALIGN(4);
        _LINKER_INTERN_init_array_end_ = .;
        . = ALIGN(4);
    } > flash
}
