#pragma once
// criticalSection<Tag>(f [, pred]): run f with its data excluded from everything else that uses the
// same tag - an ISR on this core, the other core, another host thread - and hand back exactly what
// f returns. What "excluded" costs is a policy:
//
//   one core          PRIMASK (IrqMask): the tag is ignored, masking interrupts excludes everything
//   two cores         PRIMASK, then a spinlock per tag (IrqMaskAndLock)
//   host              a std::mutex per tag (MutexPerTag), so TSan sees real locks
//
//     struct RxTag {};                                               // names the data, not the code
//     auto const b = Kvasir::criticalSection<RxTag>([&] { return rx.pop(); });
//     Kvasir::criticalSection([&] { ++shared; });                    // no tag: the one global section
//     auto const frame = Kvasir::criticalSection<RxTag>([&] { return rx.takeFrame(); },
//                                                       [&] { return rx.frameComplete(); });
//
// The two-argument form waits until pred() holds and runs f with no gap between the test and f.
// Never call it with interrupts already masked (from an ISR, inside another section): nothing could
// ever make pred true.
//
// f's result is built inside the section; a reference into the guarded data outlives it.
//
// Another policy is the image's choice, a late hook (the table in kvasir/Port/Hooks.hpp), specialised from any header,
// before or after this one:
//
//     template<>
//     struct Kvasir::Atomic::CriticalSectionPolicy<Kvasir::Tag::User> { using type = MyPolicy; };
//
// criticalSection reads it through Port::LateT with its own empty pack, so the specialisation is taken at
// instantiation; the return type is spelled out (f's own), not deduced, because deducing it would instantiate the
// body at the first call. Every translation unit of a firmware must see the same specialisation (put it in the header
// they all include); nothing can check that.
//
// Flash/XIP code that masks interrupts because XIP is off is not a critical section over data: it masks with
// Core::IrqGuard (core/InterruptMask.hpp), which is what Policy::IrqMask runs under too.

#include "kvasir/Atomic/Atomic.hpp"
#include "kvasir/Atomic/Spinlock.hpp"
#include "kvasir/Atomic/Sync.hpp"
#include "kvasir/Common/Tags.hpp"
#include "kvasir/Port/Hooks.hpp"

#include <concepts>
#include <utility>

#ifdef __arm__
    #include "core/Barrier.hpp"
#else
    #include <mutex>
    #include <thread>
#endif

namespace Kvasir::Atomic {

namespace Detail {
    struct ProbeTag {};

    [[gnu::always_inline]] inline void waitForInterrupt() {
#ifdef __arm__
        Core::wfi();
#else
        std::this_thread::yield();
#endif
    }
}   // namespace Detail

// The tag of an untagged section: every untagged section excludes every other one, under any policy.
struct GlobalSection {};

// A policy runs a callable with its exclusion held, per tag, and hands back exactly what the
// callable returns (the probe returns int&&: a policy that decays the result does not satisfy this).
template<typename P>
concept CriticalSectionPolicyShape = requires(int&& (*f)(), bool (*pred)()) {
    { P::template run<Detail::ProbeTag>(f) } -> std::same_as<int&&>;
    { P::template run<Detail::ProbeTag>(f, pred) } -> std::same_as<int&&>;
};

// The policies carry the KVASIR_MULTICORE ABI tag (Sync.hpp): Policy::Default switches with it.
inline namespace KVASIR_ATOMIC_ABI { namespace Policy {

    // One core. The tag is ignored: masking interrupts excludes everything.
    struct IrqMask {
        template<typename Tag,
                 typename F>
        [[gnu::always_inline]] static decltype(auto) run(F&& f) {
            Core::IrqGuard const guard{};
            return std::forward<F>(f)();
        }

        // Wait for pred, then run f, with no gap between the test and f. WFI runs with interrupts
        // still masked: a pending interrupt ends it anyway (Armv6-M ARM DDI0419E, B1 "WFI": with
        // PRIMASK.PM set, an exception of higher priority than any active one exits WFI; Armv8-M
        // ARM DDI0553B.y B2.1.2 RHRMJ: wakeup "ignoring the effect of PRIMASK"), so an interrupt
        // between the test and the WFI is not slept through. Unmasking lets the handler run.
        template<typename Tag,
                 typename F,
                 typename Pred>
        static decltype(auto) run(F&&    f,
                                  Pred&& pred) {
            while(true) {
                Core::IrqGuard const guard{};
                if(pred()) { return std::forward<F>(f)(); }
                Detail::waitForInterrupt();
            }
        }
    };

    // Two cores: this core's interrupt mask, then a lock per tag, so nothing on this core can
    // pre-empt the holder and spin on the lock it holds (Spinlock.hpp's rule).
    struct IrqMaskAndLock {
        template<typename Tag>
        static inline IrqSpinlock section{};

        template<typename Tag,
                 typename F>
        [[gnu::always_inline]] static decltype(auto) run(F&& f) {
            LockGuard const guard{section<Tag>};
            return std::forward<F>(f)();
        }

        // No WFI: what makes pred true may come from the other core without an interrupt here.
        template<typename Tag,
                 typename F,
                 typename Pred>
        static decltype(auto) run(F&&    f,
                                  Pred&& pred) {
            while(true) {
                LockGuard const guard{section<Tag>};
                if(pred()) { return std::forward<F>(f)(); }
            }
        }
    };

#ifndef __arm__
    // Host: a mutex per tag.
    struct MutexPerTag {
        template<typename Tag>
        static inline std::mutex mutex{};

        template<typename Tag,
                 typename F>
        static decltype(auto) run(F&& f) {
            std::lock_guard const lock{mutex<Tag>};
            return std::forward<F>(f)();
        }

        template<typename Tag,
                 typename F,
                 typename Pred>
        static decltype(auto) run(F&&    f,
                                  Pred&& pred) {
            while(true) {
                {
                    std::unique_lock const lock{mutex<Tag>};
                    if(pred()) { return std::forward<F>(f)(); }
                }
                std::this_thread::yield();
            }
        }
    };

    using Default = MutexPerTag;
#elif defined(KVASIR_MULTICORE) && KVASIR_MULTICORE
    using Default = IrqMaskAndLock;
#else
    using Default = IrqMask;
#endif

    static_assert(CriticalSectionPolicyShape<IrqMask>);
    static_assert(CriticalSectionPolicyShape<IrqMaskAndLock>);
    static_assert(CriticalSectionPolicyShape<Default>);
}}   // namespace KVASIR_ATOMIC_ABI::Policy

// The image's policy (header comment): type, one of Policy:: or the application's own.
template<typename T, typename... Ts>
struct CriticalSectionPolicy {
    using type = Policy::Default;
};

template<typename Tag = GlobalSection,
         typename... Dummy,
         typename F,
         typename... Pred>
    requires(sizeof...(Dummy) == 0 && sizeof...(Pred) < 2)
[[gnu::always_inline]] inline auto criticalSection(F&& f,
                                                   Pred&&... pred)
  -> decltype(std::forward<F>(f)()) {
    using P = typename Port::LateT<CriticalSectionPolicy<::Kvasir::Tag::User>, Dummy...>::type;
    static_assert(CriticalSectionPolicyShape<P>,
                  "CriticalSectionPolicy<Tag::User>::type is not a critical-section policy "
                  "(CriticalSection.hpp)");
    return P::template run<Tag>(std::forward<F>(f), std::forward<Pred>(pred)...);
}

}   // namespace Kvasir::Atomic

namespace Kvasir { using Atomic::criticalSection; }   // namespace Kvasir
