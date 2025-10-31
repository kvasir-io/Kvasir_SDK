/*
 * Kvasir Embedded Framework - Memory Layout Configuration
 *
 * This file provides common linker definitions for both flash-based and
 * RAM-only configurations. It is included by chip-specific linker scripts
 * that define the MEMORY regions.
 *
 * The actual memory layout differs based on which configuration is used:
 * - Flash-based: Code in flash (XIP), data in RAM
 * - RAM-only: Everything in RAM (no flash memory region)
 *
 * =============================================================================
 * FLASH-BASED CONFIGURATION
 * =============================================================================
 *
 * Chip-specific linker script must define MEMORY regions and include:
 *   INCLUDE common_flash.ld
 *   INCLUDE common_ram.ld
 *   INCLUDE common.ld
 *
 * FLASH:
 * ┌─────────────────────────────────────┐ ORIGIN(flash)
 * │ .text (code & read-only data)       │
 * │  - .core_vectors (ISR table)        │
 * │  - .after_vectors                   │
 * │  - .text* (program code)            │
 * │  - .rodata* (const data)            │
 * │  - .init_array (C++ constructors)   │
 * ├─────────────────────────────────────┤ _LINKER_INTERN_data_start_flash_
 * │ .data (flash copy of init data)     │
 * └─────────────────────────────────────┘ _LINKER_INTERN_data_end_flash_
 *
 * RAM:
 * ┌─────────────────────────────────────┐ ORIGIN(ram)
 * │ .noInitLowRam (special low RAM)     │ <- Not cleared at startup
 * ├─────────────────────────────────────┤ _LINKER_INTERN_stack_start_
 * │ .stack (early placement)            │
 * │  - .stackProtector                  │
 * │  - Stack space                      │ <- Size from two-stage link
 * ├─────────────────────────────────────┤ _LINKER_INTERN_stack_end_
 * │ .data (initialized globals)         │ <- LMA=flash, VMA=RAM, copied at startup
 * ├─────────────────────────────────────┤ _LINKER_INTERN_bss_start_
 * │ .bss (zero-initialized globals)     │ <- Cleared at startup
 * ├─────────────────────────────────────┤ _LINKER_INTERN_noInit_start_
 * │ .noInit (uninitialized data)        │ <- Not touched at startup
 * ├─────────────────────────────────────┤ _LINKER_INTERN_heap_start_
 * │ .heap (dynamic allocation)          │
 * └─────────────────────────────────────┘ _LINKER_INTERN_heap_end_
 *
 * Key: .data has AT(_LINKER_INTERN_data_start_flash_) - startup must copy from flash!
 *
 * =============================================================================
 * RAM-ONLY CONFIGURATION
 * =============================================================================
 *
 * Chip-specific linker script must define MEMORY region (RAM only) and include:
 *   INCLUDE common_ram_only.ld
 *   INCLUDE common.ld
 *
 * NO FLASH MEMORY REGION!
 *
 * RAM:
 * ┌─────────────────────────────────────┐ ORIGIN(ram)
 * │ .text (code executed from RAM)      │
 * │  - .core_vectors (ISR table)        │
 * │  - .after_vectors                   │
 * │  - .text* (program code)            │
 * │  - .rodata* (const data)            │
 * │  - .init_array (C++ constructors)   │
 * ├─────────────────────────────────────┤ _LINKER_INTERN_data_start_
 * │ .data (initialized globals)         │ <- No AT(), already in RAM!
 * ├─────────────────────────────────────┤ _LINKER_INTERN_bss_start_
 * │ .bss (zero-initialized globals)     │ <- Cleared at startup
 * ├─────────────────────────────────────┤ _LINKER_INTERN_noInit_start_
 * │ .noInit (uninitialized data)        │ <- Not touched at startup
 * ├─────────────────────────────────────┤ _LINKER_INTERN_stack_start_
 * │ .stack (late placement)             │
 * │  - .stackProtector                  │
 * │  - Stack space                      │ <- Size from two-stage link
 * ├─────────────────────────────────────┤ _LINKER_INTERN_heap_start_
 * │ .heap (dynamic allocation)          │
 * └─────────────────────────────────────┘ _LINKER_INTERN_heap_end_
 *
 * Key: .data has NO AT() - startup skips copy (check _LINKER_data_start_flash_ == 0)
 *      _LINKER_INTERN_rom_start/size/end = 0 (no flash)
 *
 * =============================================================================
 * MODULAR FILE STRUCTURE
 * =============================================================================
 *
 * This linker setup provides reusable building blocks for chip-specific
 * linker scripts. The chip-specific script defines MEMORY regions and
 * includes the appropriate composition files.
 *
 * Composition Files (to be included by chip-specific scripts):
 *   common_flash.ld     -> Includes common_text_flash.ld (for flash config)
 *   common_ram.ld       -> Includes common_data_flash.ld + common_sections.ld + common_heap.ld
 *   common_ram_only.ld  -> Includes all needed files for RAM-only config
 *   common.ld           -> This file (always include last)
 *
 * Building Blocks (low-level section definitions):
 *   common_text_flash.ld     = .text in flash memory
 *   common_text_ram.ld       = .text in RAM memory
 *   common_text_body.inc.ld  = Shared .text section body (included by both)
 *   common_data_flash.ld     = .noInitLowRam, .stack (early), .data (with AT)
 *   common_data_ram_only.ld  = .data (without AT), flash symbols = 0
 *   common_data_body.inc.ld  = Shared .data section body (included by both)
 *   common_sections.ld       = .bss, .noInit (shared by both configs)
 *   common_stack_heap.ld     = .stack (late), .heap (for RAM-only)
 *   common_heap.ld           = .heap (for flash config)
 *   common.ld                = This file (symbols, assertions, /DISCARD/)
 *
 * =============================================================================
 * USAGE EXAMPLE
 * =============================================================================
 *
 * Flash-based chip-specific linker script example:
 *   MEMORY {
 *       flash (rx) : ORIGIN = 0x10000000, LENGTH = 4M
 *       ram (rwx)  : ORIGIN = 0x20000000, LENGTH = 520K
 *   }
 *   INCLUDE common_flash.ld
 *   INCLUDE common_ram.ld
 *   INCLUDE common.ld
 *
 * RAM-only chip-specific linker script example:
 *   MEMORY {
 *       ram (rwx) : ORIGIN = 0x20000000, LENGTH = 520K
 *   }
 *   INCLUDE common_ram_only.ld
 *   INCLUDE common.ld
 *
 * =============================================================================
 * NOTES
 * =============================================================================
 *
 * Stack Placement Differs:
 *   - Flash config: stack placed EARLY (after .noInitLowRam, before .data)
 *   - RAM-only: stack placed LATE (after .noInit, before .heap)
 *
 * Two-Stage Linking (../cmake/tools/two_stage_link.py):
 *   Stage 1: Link with cmake_stack_size_extra=0, measure actual section sizes
 *   Stage 2: Calculate remaining RAM, set cmake_stack_size_extra, relink
 *   Result: Stack grows to fill all available RAM after other sections
 *
 * SAFETY FEATURES:
 *   - Fill pattern 0xFFDEFFDE (ARM "udf #255") triggers HardFault on runaway code
 *   - Stack and heap grow AWAY from each other for better memory safety
 *   - Assertions verify memory layout consistency at link time
 */

SECTIONS {
    /DISCARD/ :
    {
        *(.ARM.exidx*)
    }
}

_LINKER_INTERN_min_stack_size_ = cmake_min_stack_size;

/* ROM/Flash symbols defined in configuration-specific files */
/* RAM definitions */
_LINKER_INTERN_ram_start_   = ORIGIN(ram);
_LINKER_INTERN_ram_size_    = LENGTH(ram);
_LINKER_INTERN_ram_end_     = _LINKER_INTERN_ram_start_ + _LINKER_INTERN_ram_size_;

_LINKER_INTERN_heap_size_           = _LINKER_INTERN_heap_end_ - _LINKER_INTERN_heap_start_;
_LINKER_INTERN_stack_size_          = _LINKER_INTERN_stack_end_ - _LINKER_INTERN_stack_start_;
_LINKER_INTERN_stackProtector_size_ = _LINKER_INTERN_stackProtector_end_ - _LINKER_INTERN_stack_start_;

/* Flash-based assertions (skip for RAM-only when flash addresses are zero) */
ASSERT( _LINKER_INTERN_data_start_flash_ == 0 || _LINKER_INTERN_init_array_end_ == _LINKER_INTERN_data_start_flash_,"ERROR: corrupted linker file - data and text not consecutive");
ASSERT( _LINKER_INTERN_data_start_flash_ == 0 || _LINKER_INTERN_data_end_flash_ - _LINKER_INTERN_data_start_flash_
     == _LINKER_INTERN_data_end_ - _LINKER_INTERN_data_start_,"ERROR: corrupted linker file - data in RAM smaller than in flash");

ASSERT( _LINKER_INTERN_min_stack_size_ <= _LINKER_INTERN_stack_size_ - _LINKER_INTERN_stackProtector_size_,"ERROR: not enough space for stack");
/*ASSERT(_LINKER_INTERN_init_array_start_ ==  _LINKER_INTERN_init_array_end_,"ERROR: global init stuff not supported");*/

/* Low RAM check (only if noInitLowRam section exists) */
ASSERT( !DEFINED(_LINKER_INTERN_noInitLowRam_end_) || 0xffff > _LINKER_INTERN_noInitLowRam_end_ - _LINKER_INTERN_ram_start_, "ERROR: low RAM too full");

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
