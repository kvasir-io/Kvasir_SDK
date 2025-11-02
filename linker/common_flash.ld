/* Flash-based execution: .text section in flash memory */

SECTIONS {
    . = ORIGIN(flash);

    .vectors : {
        INCLUDE common_vectors_body.inc.ld
    } > flash

    .text : {
        INCLUDE common_text_body.inc.ld
    } > flash

    _LINKER_INTERN_data_start_flash_ = .;
}

/* Flash/ROM memory symbols */
_LINKER_INTERN_rom_start_   = ORIGIN(flash);
_LINKER_INTERN_rom_size_    = LENGTH(flash);
_LINKER_INTERN_rom_end_     = _LINKER_INTERN_rom_start_ + _LINKER_INTERN_rom_size_;
