/*
 * Kvasir Embedded Framework - Memory Layout Configuration
 *
 * MEMORY MAP OVERVIEW:
 * ===================
 *
 * FLASH MEMORY LAYOUT:
 * ┌─────────────────────────────────────┐ <- ORIGIN(flash)
 * │ Vector Table (.core_vectors)        │
 * │ Post-Vector Code (.after_vectors)   │
 * │ Program Code (.text)                │
 * │ Read-Only Data (.rodata)            │
 * │ C++ Constructor Array (.init_array) │
 * └─────────────────────────────────────┘ <- _LINKER_INTERN_data_start_flash_
 * │ Initialized Data (flash copy)       │
 * └─────────────────────────────────────┘ <- _LINKER_INTERN_data_end_flash_
 *
 * RAM MEMORY LAYOUT:
 * ┌─────────────────────────────────────┐ <- ORIGIN(ram)
 * │ Low RAM (.noInitLowRam) [0-64KB]    │
 * ├─────────────────────────────────────┤ <- _LINKER_INTERN_stack_start_
 * │ Stack Protection (.stackProtector)  │
 * │ Main Stack (grows downward)         │
 * └─────────────────────────────────────┘ <- _LINKER_INTERN_stack_end_
 * │ Initialized Data (.data)            │ <- Copied from flash at startup
 * ├─────────────────────────────────────┤
 * │ Zero-Initialized Data (.bss)        │ <- Cleared at startup
 * ├─────────────────────────────────────┤
 * │ Uninitialized Data (.noInit)        │ <- Not touched at startup
 * ├─────────────────────────────────────┤ <- _LINKER_INTERN_heap_start_
 * │ Heap (grows upward)                 │
 * └─────────────────────────────────────┘ <- _LINKER_INTERN_heap_end_
 *
 * NOTE: This is an unusual layout where stack and heap grow AWAY from each
 * other (not towards each other as in typical implementations). Stack is
 * placed at low addresses and heap at high addresses, providing better
 * memory safety and predictable behavior for embedded systems.
 *
 * SAFETY FEATURES:
 * - Fill pattern 0xFFDEFFDE (ARM "udf #255") triggers HardFault on runaway code
 */

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

ASSERT( _LINKER_INTERN_init_array_end_ == _LINKER_INTERN_data_start_flash_,"ERROR: corrupted linker file - data and text not consecutive");
ASSERT( _LINKER_INTERN_data_end_flash_ - _LINKER_INTERN_data_start_flash_
     == _LINKER_INTERN_data_end_ - _LINKER_INTERN_data_start_,"ERROR: corrupted linker file - data in RAM smaller than in flash");

ASSERT( _LINKER_INTERN_min_stack_size_ <= _LINKER_INTERN_stack_size_ - _LINKER_INTERN_stackProtector_size_,"ERROR: not enough space for stack");
/*ASSERT(_LINKER_INTERN_init_array_start_ ==  _LINKER_INTERN_init_array_end_,"ERROR: global init stuff not supported");*/

ASSERT(0xffff > _LINKER_INTERN_noInitLowRam_end_ - _LINKER_INTERN_ram_start_, "ERROR: low RAM too full");

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
