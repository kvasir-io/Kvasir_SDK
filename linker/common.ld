SECTIONS {
    /DISCARD/ :
    {
        *(.ARM.exidx*)
    }
}

_LINKER_INTERN_min_stack_size_ = cmake_min_stack_size;

_LINKER_INTERN_rom_start_   = ORIGIN(flash);
_LINKER_INTERN_rom_size_    = LENGTH(flash);
_LINKER_INTERN_rom_end_     = _LINKER_INTERN_rom_start_ + _LINKER_INTERN_rom_size_;

_LINKER_INTERN_ram_start_   = ORIGIN(ram);
_LINKER_INTERN_ram_size_    = LENGTH(ram);
_LINKER_INTERN_ram_end_     = _LINKER_INTERN_ram_start_ + _LINKER_INTERN_ram_size_;

_LINKER_INTERN_heap_size_           = _LINKER_INTERN_heap_end_ - _LINKER_INTERN_heap_start_;
_LINKER_INTERN_stack_size_          = _LINKER_INTERN_stack_end_ - _LINKER_INTERN_stack_start_;
_LINKER_INTERN_stackProtector_size_ = _LINKER_INTERN_stackProtector_end_ - _LINKER_INTERN_stack_start_;

ASSERT( _LINKER_INTERN_init_array_end_ == _LINKER_INTERN_data_start_flash_,"ERROR: corrupted linker file data and text not consecutive");
ASSERT( _LINKER_INTERN_data_end_flash_ - _LINKER_INTERN_data_start_flash_
     == _LINKER_INTERN_data_end_ - _LINKER_INTERN_data_start_,"ERROR: corrupted linker file data in ram smaller then in flash");

ASSERT( _LINKER_INTERN_min_stack_size_ <= _LINKER_INTERN_stack_size_ - _LINKER_INTERN_stackProtector_size_,"ERROR: not enough space for stack");
/*ASSERT(_LINKER_INTERN_init_array_start_ ==  _LINKER_INTERN_init_array_end_,"ERROR: global init stuff not supported");*/

ASSERT(0xffff > _LINKER_INTERN_noInitLowRam_end_ - _LINKER_INTERN_ram_start_, "ERROR: low ram to full");

_LINKER_heap_start_       = _LINKER_INTERN_heap_start_;
_LINKER_heap_end_         = _LINKER_INTERN_heap_end_;
_LINKER_stack_start_      = _LINKER_INTERN_stack_start_;
_LINKER_stack_end_        = _LINKER_INTERN_stack_end_;
_LINKER_init_array_start_ = _LINKER_INTERN_init_array_start_;
_LINKER_init_array_end_   = _LINKER_INTERN_init_array_end_;
_LINKER_data_start_flash_ = _LINKER_INTERN_data_start_flash_;
_LINKER_data_end_flash_   = _LINKER_INTERN_data_end_flash_;
_LINKER_data_start_       = _LINKER_INTERN_data_start_;
_LINKER_bss_end_          = _LINKER_INTERN_bss_end_;
