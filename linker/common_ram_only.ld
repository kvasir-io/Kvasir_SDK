/* RAM-only execution: all sections in RAM, no flash copy needed */
INCLUDE common_text_ram.ld
INCLUDE common_data_ram_only.ld
INCLUDE common_sections.ld
INCLUDE common_stack_heap.ld

/* No flash memory in RAM-only configuration */
_LINKER_INTERN_rom_start_   = 0;
_LINKER_INTERN_rom_size_    = 0;
_LINKER_INTERN_rom_end_     = 0;
