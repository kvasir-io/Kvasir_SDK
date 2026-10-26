#pragma once
// One check per piece of the port contract: a missing piece is a static_assert that names it and the package that
// should provide it, not an error deep inside Startup. Start<> runs checkCore0<S>(); SecondaryCore and BootGuard
// assert theirs where they use them.
//
// The chip's hooks and traits are looked up late, through Port::LateT on the Startup type. The core functions are
// plain functions: the caller passes the type a call of them has where it is written (after the core header), so a
// fallback from Port/Core.hpp shows up as Missing.
#include "kvasir/Common/Tags.hpp"
#include "kvasir/Port/Core.hpp"
#include "kvasir/Port/Hooks.hpp"
#include "kvasir/Port/Interrupts.hpp"

#include <concepts>
#include <cstdint>
#include <type_traits>

namespace Kvasir::Port {
// the chip's first init step (chip/StartUp.hpp)
template<typename D>
concept ChipFirstInit = requires { LateT<Startup::FirstInitStep<Tag::User>, D>{}(); };

// the chip's interrupt range (chip/Interrupt.hpp)
template<typename D>
concept ChipInterrupts = requires {
    { LateT<Nvic::InterruptOffsetTraits<Kvasir::Tag::User>, D>::begin } -> std::convertible_to<int>;
    { LateT<Nvic::InterruptOffsetTraits<Kvasir::Tag::User>, D>::end } -> std::convertible_to<int>;
};

// a core function the port needs: F = decltype(the call), Missing when only Port/Core.hpp's fallback was found
template<typename F>
concept CoreProvides = !std::is_same_v<F, Missing>;

// what a secondary core does for itself (the chip's SecondaryCoreInit<Tag::User>, chip/StartUp.hpp)
template<typename Init>
concept ChipSecondaryCore = requires {
    { Init::cpacrEnable } -> std::convertible_to<std::uint32_t>;
};

// the chip's reset kind (its PM::resetKind(), Port/ResetKind.hpp has the fallback): F = decltype(PM::resetKind()),
// taken where the caller is written - after the chip's declaration
template<typename F>
concept ChipResetKind = std::is_same_v<F, ResetKind>;

// D: the Startup; Clean = decltype(Startup::Core::cleanCore()),
// NvicClear = decltype(Nvic::clearAllEnablesAndPending()).
template<typename D,
         typename Clean,
         typename NvicClear>
consteval bool checkCore0() {
    static_assert(ChipFirstInit<D>,
                  "Kvasir port: the chip package provides no Startup::FirstInitStep<Tag::User> "
                  "(chip/StartUp.hpp) - "
                  "include the chip before naming a Startup");
    static_assert(
      ChipInterrupts<D>,
      "Kvasir port: the chip package provides no Nvic::InterruptOffsetTraits<Tag::User> with "
      "begin/end (chip/Interrupt.hpp)");
    static_assert(
      CoreProvides<Clean>,
      "Kvasir port: the core package provides no Startup::Core::cleanCore() (core/StartUp.hpp)");
    static_assert(CoreProvides<NvicClear>,
                  "Kvasir port: the core package provides no Nvic::clearAllEnablesAndPending() "
                  "(core_cortex_common "
                  "Nvic.hpp)");
    return true;
}
}   // namespace Kvasir::Port
