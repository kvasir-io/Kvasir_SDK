#pragma once

#include <cstddef>
#include <cstdint>

// The symbols the linker scripts (linker/common_*.ld) define, declared once: a second declaration
// elsewhere is a -Wredundant-decls under gcc.
extern "C" {
extern void _LINKER_vectors_start_();   // the boot core's vector table (.core_vectors)
// .after_vectors, right behind that table: the RP2350's picobin block, which IMAGE_CRC leaves out there (the chip's
// TARGET_IMAGE_CRC_EXCLUDE); start == end on a chip that puts nothing in it - which the compiler does not believe
// of two functions: compare the addresses as run-time values (the asm that makes them one)
extern void _LINKER_INTERN_after_vectors_start_();
extern void _LINKER_INTERN_after_vectors_end_();

extern void _LINKER_stack_start_();   // low address - the stack grows DOWN to here
extern void _LINKER_stack_end_();     // high address - the initial stack pointer

// The secondary core's stack (.stack1), sized by CORE1_STACK_SIZE; start == end when unset
extern void _LINKER_stack1_start_();
extern void _LINKER_stack1_end_();

// The heap (.heap, sized by HEAP_SIZE); start == end without one
extern void _LINKER_heap_start_();
extern void _LINKER_heap_end_();

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
// `top` and `bottom` are the linker symbols themselves, for an asm "i" operand: their addresses
// are link-time constants, so code can compare them without a load.
struct PrimaryStackTop {
    static constexpr auto top    = &_LINKER_stack_end_;
    static constexpr auto bottom = &_LINKER_stack_start_;

    static constexpr std::uintptr_t value() {
        return reinterpret_cast<std::uintptr_t>(&_LINKER_stack_end_);
    }
};

struct Core1StackTop {
    static constexpr auto top    = &_LINKER_stack1_end_;
    static constexpr auto bottom = &_LINKER_stack1_start_;

    static constexpr std::uintptr_t value() {
        return reinterpret_cast<std::uintptr_t>(&_LINKER_stack1_end_);
    }
};
}   // namespace Kvasir::Startup
