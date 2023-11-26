SECTIONS{
    . = ORIGIN(flash);
    .text : {
        FILL(0xFFDEFFDE); /*"udf #255" instruction as fill pattern so we end up in Hardfault handler when PC goes wild*/
        . = ALIGN(4);
        KEEP(*(.core_vectors*))
        *(.text*)
        *(.rodata*)
        /**(.eh_frame*)*/
        /**(.ARM.extab*)*/
        . = ALIGN(4);
        _LINKER_INTERN_init_array_start_ = .;
        . = ALIGN(4);
        KEEP(*(.init_array*))
        . = ALIGN(4);
        _LINKER_INTERN_init_array_end_ = .;
        . = ALIGN(4);
    } > flash
}
