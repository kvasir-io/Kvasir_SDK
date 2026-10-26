#pragma once
// Faults on purpose: for an example, a test image or a bench hook that shows a fault handler, a fault record or the
// MPU at work. Each is one access through Kvasir::Memory, always inlined, so the record's pc is the caller's:
//
//     Kvasir::Fault::Inject::busFaultLoad();              // one word load from the chip's Port::UnmappedAddress
//     Kvasir::Fault::Inject::busFaultStore();             // one word store there
//     Kvasir::Fault::Inject::writeToCode(address);        // one word store of 0xFFFFFFFF into code or a constant
//
// busFaultLoad: a precise BusFault with BFAR = the address on Armv7-M/Armv8-M (PRECISERR, BFARVALID), a HardFault on
// the M0+. busFaultStore: the same bus error, imprecise where the core buffers the store (IMPRECISERR, no BFAR).
// writeToCode: a MemManage DACCVIOL with MMFAR = the address where the MPU holds the code read-only
// (Mpu::ImageProtection); without that, what the memory does with a store (XIP flash: nothing, or the XIP cache).
//
// The unmapped address is the chip package's (the hook Port::UnmappedAddress, kvasir/Port/Hooks.hpp): a chip without
// one does not compile busFaultLoad()/busFaultStore().
#include "kvasir/Common/Tags.hpp"
#include "kvasir/Port/Hooks.hpp"
#include "kvasir/Register/Memory.hpp"

#include <cstdint>

namespace Kvasir::Fault::Inject {
// the chip's address no bus slave answers at; Dummy... defers the lookup to the caller's instantiation
template<typename... Dummy>
inline constexpr std::uintptr_t unmappedAddress
  = Port::LateT<Port::UnmappedAddress<Tag::User>, Dummy...>::value;

// one word load from Port::UnmappedAddress: a bus error
template<typename... Dummy>
[[gnu::always_inline]] inline void busFaultLoad() {
    (void)Memory::load<std::uint32_t>(unmappedAddress<Dummy...>);
}

// one word store of `value` to Port::UnmappedAddress: a bus error (imprecise where the store is buffered)
template<typename... Dummy>
[[gnu::always_inline]] inline void busFaultStore(std::uint32_t value = 1U) {
    Memory::store<std::uint32_t>(unmappedAddress<Dummy...>, value);
}

// one word store of 0xFFFFFFFF (erased flash) at `address`, the word-aligned address of a function or a constant: the
// MPU's DACCVIOL where the code is read-only
[[gnu::always_inline]] inline void writeToCode(std::uintptr_t address) {
    Memory::store<std::uint32_t>(address, 0xFFFF'FFFFU);
}
}   // namespace Kvasir::Fault::Inject
