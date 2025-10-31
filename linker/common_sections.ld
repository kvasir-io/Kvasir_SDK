/* Truly shared RAM sections (same relative position in both flash and RAM-only configs) */

SECTIONS {
    /* Zero-initialized data */
    .bss (NOLOAD) : AT(.) {
        . = ALIGN(4);
        _LINKER_INTERN_bss_start_ = .;
        . = ALIGN(4);
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _LINKER_INTERN_bss_end_ = .;
        . = ALIGN(4);
    } > ram

    /* Uninitialized data (not touched at startup) */
    .noInit (NOLOAD) : AT(.) {
        . = ALIGN(4);
        _LINKER_INTERN_noInit_start_ = .;
        . = ALIGN(4);
        *(.noInit*)
        . = ALIGN(4);
        _LINKER_INTERN_noInit_end_ = .;
        . = ALIGN(4);
    } > ram
}
