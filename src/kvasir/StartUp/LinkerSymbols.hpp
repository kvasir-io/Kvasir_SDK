#pragma once

#include <cstddef>
#include <cstdint>

// The symbols the linker scripts (linker/common_*.ld) define, declared once: a second declaration
// elsewhere is a -Wredundant-decls under gcc.
extern "C" {
extern void _LINKER_stack_start_();   // low address - the stack grows DOWN to here
extern void _LINKER_stack_end_();     // high address - the initial stack pointer

using InitFunc = void (*)();
extern InitFunc _LINKER_init_array_start_;
extern InitFunc _LINKER_init_array_end_;

extern std::uintptr_t _LINKER_data_start_flash_;
extern std::uintptr_t _LINKER_data_start_;
extern std::size_t    _LINKER_data_size_;

extern std::uintptr_t _LINKER_bss_start_;
extern std::size_t    _LINKER_bss_size_;
}
