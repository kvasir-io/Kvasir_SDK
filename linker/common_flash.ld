/* Flash-based execution: .text section in flash memory */
INCLUDE common_text_flash.ld

/* Flash/ROM memory symbols */
_LINKER_INTERN_rom_start_   = ORIGIN(flash);
_LINKER_INTERN_rom_size_    = LENGTH(flash);
_LINKER_INTERN_rom_end_     = _LINKER_INTERN_rom_start_ + _LINKER_INTERN_rom_size_;
