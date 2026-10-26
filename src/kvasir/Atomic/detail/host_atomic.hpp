#pragma once

#include <cstdint>

// The target's PRIMASK API (core/InterruptMask.hpp) as no-ops: the same names and shape - constexpr, IrqGuard neither
// copied nor moved, IrqState a plain word (0: never masked) - so code written against it compiles on the host; there
// is no interrupt to mask here. Pinned by tests/critical_section_tests.cpp (the target's twin of those asserts:
// core_cortex_common tests/irq_state_syntax.cpp).
namespace Kvasir::Core {
[[nodiscard]] constexpr bool irqsMasked() { return false; }

constexpr void maskIrqs() {}

constexpr void unmaskIrqs() {}

struct IrqState {
    std::uint32_t primask;

    [[nodiscard]] static constexpr IrqState saveAndMask() { return IrqState{0U}; }

    constexpr void restore() const {}

    [[nodiscard]] constexpr bool wasMasked() const { return primask != 0U; }
};

class IrqGuard {
    IrqState state_;

public:
    constexpr IrqGuard() : state_{IrqState::saveAndMask()} {}

    constexpr ~IrqGuard() { state_.restore(); }

    IrqGuard(IrqGuard const&)            = delete;
    IrqGuard& operator=(IrqGuard const&) = delete;

    [[nodiscard]] constexpr bool wasMasked() const { return state_.primask != 0U; }
};
}   // namespace Kvasir::Core

namespace Kvasir::Atomic {
inline void onOtherCoreStopped() {}
}   // namespace Kvasir::Atomic
