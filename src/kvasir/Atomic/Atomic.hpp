#pragma once
// The interrupt mask the Atomic blocks and the drivers run under, and what a stopped other core means for the atomic
// shim. The shim itself (the __atomic_* / __sync_* entry points the compiler calls) is defined once per image, in the
// TU with main: kvasir/StartUp/Runtime/LibAtomic.hpp.
#ifdef __arm__
    #include "core/InterruptMask.hpp"

namespace Kvasir::Atomic {
// Release the shim's cross-core lock the stopped core may have held (SecondaryCore::reset, Port::StopOtherCore). A
// no-op on a single-core image. Defined in StartUp/Runtime/LibAtomic.hpp.
void onOtherCoreStopped();
}   // namespace Kvasir::Atomic
#else
    #include "detail/host_atomic.hpp"
#endif
