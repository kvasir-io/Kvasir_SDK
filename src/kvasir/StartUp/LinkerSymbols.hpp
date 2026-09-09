#pragma once

#include <cstddef>
#include <cstdint>

// The symbols the linker scripts (linker/common_*.ld) define, declared once: a second declaration
// elsewhere is a -Wredundant-decls under gcc.
extern "C" {
extern void _LINKER_stack_start_();   // low address - the stack grows DOWN to here
extern void _LINKER_stack_end_();     // high address - the initial stack pointer

// The secondary core's stack (.stack1), sized by CORE1_STACK_SIZE; start == end when unset
extern void _LINKER_stack1_start_();
extern void _LINKER_stack1_end_();

using InitFunc = void (*)();
extern InitFunc _LINKER_init_array_start_;
extern InitFunc _LINKER_init_array_end_;

extern std::uintptr_t _LINKER_data_start_flash_;
extern std::uintptr_t _LINKER_data_start_;
extern std::size_t    _LINKER_data_size_;

extern std::uintptr_t _LINKER_bss_start_;
extern std::size_t    _LINKER_bss_size_;
}

// Which stack a component that needs "the top of this core's stack" (the fault handler's
// safe stack, for one) should use. Tags, so they can be template arguments.
namespace Kvasir::Startup {
struct PrimaryStackTop {
    static constexpr std::uintptr_t value() {
        return reinterpret_cast<std::uintptr_t>(&_LINKER_stack_end_);
    }
};

struct Core1StackTop {
    static constexpr std::uintptr_t value() {
        return reinterpret_cast<std::uintptr_t>(&_LINKER_stack1_end_);
    }
};
}   // namespace Kvasir::Startup
