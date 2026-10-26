#pragma once

// DMA channels as Startup resources (kvasir/StartUp/Resources.hpp). A chip's DMA instance
// provides the channels it owns, a driver claims the channels it starts transfers on; Startup
// then refuses two drivers on one channel, a driver whose instance is in no list, and (the tag
// keeps the default `coreLocal`) a driver whose instance is in the other core's list - the
// completion callback would run on the wrong core.
//
//     using Provides = brigand::list<Kvasir::Hal::DmaChannel<Regs, 0>, Kvasir::Hal::DmaChannel<Regs, 1>>;
//     using Claims   = brigand::list<Kvasir::Hal::DmaChannel<Regs, 1>>;
//
// `Controller` is the DMA controller's register set (or any type naming it), so two
// controllers' channel 0 are two resources. Each chip family's driver keeps its own claim
// helper (`Kvasir::DMA::Claims`, `Kvasir::DMAC::Claims`) with the checks only it can make.

#include "kvasir/StartUp/Resources.hpp"

namespace Kvasir { namespace Hal {
    template<typename Controller>
    struct DmaChannelTag {
        static constexpr char const* describeAs = "DMA channel";   // in a compile error
    };

    template<typename Controller, auto Ch>
    using DmaChannel = Startup::Resource<DmaChannelTag<Controller>, Ch>;
}}   // namespace Kvasir::Hal
