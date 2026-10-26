#pragma once
// The fault records, without the fault handler: what a fault leaves behind for the next boot (CrashRecord.hpp picks
// the options). The handler (FaultHandler.hpp) writes them; BootGuard, the boot report and CrashLog.hpp read them. No
// core package and no uc_log here, so the records compile on a host too.
//
// The host reads them by symbol and layout: Kvasir::Fault::lastFault and lastStack<N> (uc_log's printer decodes them,
// printer/TargetRecords.hpp, pinned by tests/records_cross_tests.cpp) - names, sections, ids and layout are fixed.
#include "kvasir/Runtime/CrashRecord.hpp"
#include "kvasir/Util/Persistent.hpp"
#include "kvasir/Util/attributes.hpp"

#include <concepts>
#include <cstdint>
#include <optional>
#include <type_traits>

namespace Kvasir { namespace Fault {
    /// The fault record: the whole state at the fault, CRC-checked (Persistent.hpp) - what takeLastFault() gives and
    /// logFault() (CrashLog.hpp) prints.
    struct RecordTag {
        static constexpr std::uint32_t id = persistentId("KFLT");
    };

    struct FullRecord {
        enum Flag : std::uint32_t {
            frameValid  = 1U << 0,   // r0-r3, r12, lr, pc, xpsr from a frameReadable() frame
            calleeSaved = 1U << 1,   // r4-r11, msp, psp, control captured at the handler's entry
            faultRegs   = 1U << 2,   // cfsr, hfsr, mmfar, bfar, shcsr (Armv7-M, Armv8-M main)
            fpFrame     = 1U << 3,   // EXC_RETURN.FType == 0: the extended (FP) frame was stacked
            stackLimits = 1U << 4,   // msplim, psplim (Armv8-M main)
            secureRegs  = 1U << 5,   // sfsr, sfar (Armv8-M with the Security Extension)
            stackCopied = 1U << 6,   // Fault::lastStack holds a snapshot
        };

        static constexpr unsigned coreShift
          = 8;   // bits 11:8 the core (0, 1 = the secondary stack)
        static constexpr unsigned archShift = 12;   // bits 15:12 the architecture (6, 7, 8)

        std::uint32_t count;   // faults since the last report; the registers are the first one's
        std::uint32_t flags;
        std::uint32_t r0, r1, r2, r3, r12, lr, pc, xpsr;   // the stacked frame
        std::uint32_t r4, r5, r6, r7, r8, r9, r10, r11;    // at the handler's entry
        std::uint32_t excReturn, msp, psp, control;
        std::uint32_t sp;   // the faulting context's SP before the frame was pushed; 0: unknown
        std::uint32_t cfsr, hfsr, mmfar, bfar, shcsr, icsr;
        std::uint32_t sfsr, sfar, msplim, psplim;
    };

    static_assert(sizeof(FullRecord) == 33 * 4,
                  "the layout the host tools decode");

    [[KVASIR_NOINIT]] inline Persistent<FullRecord, RecordTag> lastFault;

    /// A copy of the faulting stack (CrashRecord::Options::stackBytes): `bytes` valid bytes from
    /// `from`, the rest 0. lastStack<N>: N is the policy's stackBytes.
    template<std::uint16_t Bytes>
    struct StackSnapshot {
        std::uint32_t from;
        std::uint32_t bytes;
        std::uint32_t words[Bytes / 4];
    };

    struct StackTag {
        static constexpr std::uint32_t id = persistentId("KSTK");
    };

    template<std::uint16_t Bytes>
    [[KVASIR_NOINIT]] inline Persistent<StackSnapshot<Bytes>, StackTag> lastStack;

}}   // namespace Kvasir::Fault

// The format's fault part (CrashRecord.hpp): how the next boot gets the record back. The bodies are the record's own.
namespace Kvasir::CrashRecord {
template<Options O>
struct Full<O>::Fault {
    static constexpr bool clearOnPowerOn = O.clearOnPowerOn;

    static bool recorded() { return ::Kvasir::Fault::lastFault.valid(); }

    // always inlined: as a call boundary it moves the register allocation of takePrevious()'s caller
    [[gnu::always_inline]] static std::optional<::Kvasir::Fault::FullRecord> take() {
        return ::Kvasir::Fault::lastFault.take();
    }
};

// What a format's Fault part provides (an injected format is checked against it where its part is used).
template<typename F>
concept FaultPart = requires {
    { std::bool_constant<F::clearOnPowerOn>{} };
    { F::recorded() } -> std::same_as<bool>;
    { F::take() } -> std::same_as<std::optional<::Kvasir::Fault::FullRecord>>;
};
}   // namespace Kvasir::CrashRecord

namespace Kvasir { namespace Fault {
    namespace Detail {
        template<typename... Dummy>
        using FaultPartOf = typename CrashRecord::Records<FullRecord, Dummy...>::Fault;

        template<typename... Dummy>
        consteval bool faultPartOk() {
            static_assert(CrashRecord::FaultPart<FaultPartOf<Dummy...>>,
                          "the injected CrashRecord format's Fault part lacks "
                          "clearOnPowerOn, recorded() or take() -> "
                          "std::optional<Fault::FullRecord> "
                          "(CrashRecord::FaultPart, FaultRecord.hpp)");
            return true;
        }
    }   // namespace Detail

    /// The record, once: cleared by reading.
    template<typename... Dummy>
        requires(sizeof...(Dummy) == 0)
    inline std::optional<FullRecord> takeLastFault() {
        static_assert(Detail::faultPartOk<Dummy...>());
        return Detail::FaultPartOf<Dummy...>::take();
    }
}}   // namespace Kvasir::Fault
