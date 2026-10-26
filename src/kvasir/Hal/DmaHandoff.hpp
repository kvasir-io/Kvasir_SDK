#pragma once

// Memory a bus master other than the core reads and writes - a DMA controller's descriptors, a USB module's
// endpoint table and buffers - is plain memory that one side owns at a time. Ownership passes with a register
// access: the write that starts the channel or arms the endpoint hands it to the hardware, the done flag the core
// reads hands it back. Two barriers mark those points:
//
//     descriptor = {...};                      // plain stores, the core's memory
//     Kvasir::Hal::handToHardware();
//     apply(set(CHCTRLA::enable));             // from here on the hardware's
//     ...
//     if(apply(read(CHINTFLAG::tcmpl)) != 0) { // the hardware says it is done
//         Kvasir::Hal::takeFromHardware();
//         use(buffer);                         // plain loads, memcpy: the core's again
//     }
//
// Why both are needed:
//   - the compiler: a register write is a volatile store, and C++ orders volatile accesses only among themselves -
//     a plain store to the descriptor may legally move past the store that starts the DMA, a plain load of the buffer
//     ahead of the flag read. Both functions are compiler barriers for all memory.
//   - the core: the Arm memory model leaves a Normal access (RAM) and a Device access (a peripheral register)
//     unordered in either direction (Armv6-M DDI0419E A3.7.2 Figure A3-6, Armv7-M DDI0403E.e A3.7.2 Figure A3-8, the
//     Cortex-M4's own table in PM0214 2.2.2 Table 12; Armv8-M DDI0553B.y B7.11: Device's non-Reordering holds between
//     two Device accesses only, all other order comes from B7.2's ordered-before relations); a DMB orders them
//     (DDI0419E A3.7.3, DDI0403E.e A3.7.3, DDI0553B.y B7.2.11).
//     Arm's own example is this case: "the DMB instruction is required before starting DMA operations" (AN321,
//     DAI0321A 4.18).
//   - which barrier: on Armv6-M a compiler barrier alone: the Cortex-M0 and M0+ complete every
//     load and store in program order ("All use of DMB is redundant due to the inherent ordering of all loads and
//     stores on the Cortex-M0+", AN321 3.5; the M0 and M0+ "do not include a write buffer", 3.6). On every
//     other Arm core a DMB, as the architecture asks: the Cortex-M3/M4 would do without it (AN321 3.5), but the same
//     profiles include the M7 and the M33/M55, which may reorder.
//   - a data cache between the core and the RAM would need cache maintenance as well; these functions do none. The
//     SAM E5x's CMCC caches the Cortex-M4's ICode/DCode bus only (the code region: flash, QSPI; DS60001507N Table
//     10-2), not the system bus SRAM is on.
//
// A field the hardware writes while it owns the memory and that the core polls (a byte count the module moves, a
// descriptor's valid bit) is read with loadHardwareField(): one load each call, never kept in a register. It does not
// order anything: a poll that ends in "done" is followed by takeFromHardware() before the rest is read.
//
// On a host (the mock register tests) the two are sequentially consistent thread fences.

#include "kvasir/Port/Capabilities.hpp"

#include <atomic>
#include <memory>
#include <type_traits>

namespace Kvasir { namespace Hal {
    namespace Detail {
        [[gnu::always_inline]] inline void dmaOrderBarrier() {
#if KVASIR_CORE_ARMV6M
            asm volatile("" ::: "memory");
#elifdef __arm__
            // its own asm, not Core::dmb(): Hal is the port layer, below the core packages (tools/layers.toml)
            asm volatile("dmb" ::: "memory");
#else
            std::atomic_thread_fence(std::memory_order_seq_cst);
#endif
        }
    }   // namespace Detail

    // Everything the core wrote before is visible to the hardware before the register access that follows.
    [[gnu::always_inline]] inline void handToHardware() { Detail::dmaOrderBarrier(); }

    // Nothing the core reads after it is read before the register access that came before (the done flag).
    [[gnu::always_inline]] inline void takeFromHardware() { Detail::dmaOrderBarrier(); }

    // One load of a field the hardware writes, read through a volatile reference (no cast): never folded (nothing in
    // the program may store the field - a write-back descriptor only the DMAC writes - and a plain or relaxed atomic
    // load of such a field may be folded to its initial value), never merged with the next one. That is what
    // std::atomic_ref<T const volatile>::load (C++26, P3323; libc++ 23 has none yet) promises; an aligned byte,
    // halfword or word load is single-copy atomic on every Arm core (Armv6-M DDI0419E A3.5.1, Armv7-M DDI0403E.e
    // A3.5.3), so the volatile load is all of it there. On a host (the mock tests) an atomic load.
    template<typename T>
        requires std::is_unsigned_v<T>
    [[gnu::always_inline]] inline T loadHardwareField(T const volatile& field) {
#ifdef __arm__
        return field;
#else
        return __atomic_load_n(std::addressof(field), __ATOMIC_RELAXED);
#endif
    }
}}   // namespace Kvasir::Hal
