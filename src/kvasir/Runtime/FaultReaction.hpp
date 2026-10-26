#pragma once
// How a fault ends: the image's Fault::FaultReaction, a late hook (the table in kvasir/Port/Hooks.hpp), and the stock
// reactions OnFault::{Panic, Halt}. Every Fault::HandlerFor (kvasir/Runtime/FaultHandler.hpp) of the image calls it on
// the fault stack, after the fault record and the clean-up. Apart from the handler, so a host test can call them.
#include "kvasir/Common/Tags.hpp"
#include "kvasir/Port/Hooks.hpp"
#include "kvasir/Runtime/Panic.hpp"

namespace Kvasir::Fault {
// How a fault ends, after its record and the clean-up, on the fault stack: the image's FaultReaction.
namespace OnFault {
    // The default: a fault goes the same way as every other fatal path - a panic "fault" whose site is the call
    // in the fault handler (the faulting PC is in the fault record) and whose detail is the exception number (3
    // HardFault, 4 MemManage, 5 BusFault, 6 UsageFault, 7 SecureFault); then the image's Panic::PanicReaction.
    struct Panic {
        [[noreturn,
          gnu::noinline]] void
        operator()() {
            ::Kvasir::Panic::raiseAt(::Kvasir::Panic::Cause::fault,
                                     ::Kvasir::Panic::Detail::sitePc(__builtin_return_address(0)),
                                     ::Kvasir::Panic::Detail::activeException());
        }
    };

    // A bkpt 6 loop inside the fault handler. Without halting debug a breakpoint in a HardFault handler locks
    // the core up (Armv6-M ARM DDI0419E C1.5), so only for a debug session.
    struct Halt {
        [[noreturn]] void operator()() {
#ifdef __arm__
            while(true) { asm("bkpt 6" : : :); }
#else
            __builtin_trap();
#endif
        }
    };

    // The application's `[[noreturn]] void fn()` on the fault stack, after the record and the clean-up (a reboot of its
    // own, a wait for the watchdog). A fn that returns ends in a trap.
    template<auto Fn>
    struct Call {
        [[noreturn]] void operator()() {
            Fn();
            __builtin_trap();
        }
    };
}   // namespace OnFault

// The image's fault reaction, a late hook (the table in kvasir/Port/Hooks.hpp): type, one of OnFault:: or the
// application's own (`[[noreturn]] void operator()()`, on the fault stack, interrupts as the fault left them):
//     template<>
//     struct Kvasir::Fault::FaultReaction<Kvasir::Tag::User> { using type = Kvasir::Fault::OnFault::Halt; };
// One for the image: every HandlerFor (both cores') ends in it.
template<typename T, typename... Ts>
struct FaultReaction {
    using type = OnFault::Panic;
};
}   // namespace Kvasir::Fault
