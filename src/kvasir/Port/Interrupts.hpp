#pragma once
// The chip's interrupt facts: Nvic::InterruptOffsetTraits<Tag::User>, specialised by each chip in chip/Interrupt.hpp
// (begin, end, disabled, the no* lists and FaultInterruptIndexs, most of them from the core's Core::InterruptRules).
// Declared only: a use before the chip is a named error (Port/Check.hpp) or an incomplete type. The shape of every port
// hook (kvasir/Port/Hooks.hpp's table): a class template the chip specialises for Kvasir::Tag::User, named as
// Port::LateT<InterruptOffsetTraits<Kvasir::Tag::User>, Dep> at the use - or as the default of a function or class
// template's `Traits` parameter, which is looked up where the template is used, a late lookup already (the core's
// NVIC actions; the host tests pass stand-in traits there).
#include "kvasir/Common/Tags.hpp"

namespace Kvasir { namespace Nvic {
    template<typename T,
             typename... Ts>
    struct InterruptOffsetTraits;   // specialized by each chip, in chip/Interrupt.hpp
}}   // namespace Kvasir::Nvic
