#pragma once
// The runtime most images want, as one Startup list entry: the fault handler, the stack protector, a boot-loop guard
// and the boot report, in that order. A group entry (StartUp/Entry.hpp): Startup runs its members in its place, and
// every rule, hook, Extends and Provides/Claims is theirs.
//
//     using Startup = Kvasir::Startup::Startup<HW::ClockSettings, Clock, HW::ComBackend,
//                                              Kvasir::Runtime::Standard<Clock>, HW::PinConfig>;
//
// List it right after the ComBackend: the guard and the report log. Clock is the guard's clock (a run that lasts
// healthyAfter is healthy; its turn is a Hook::MainLoop function, so the loop calls Startup::run<Hook::MainLoop>()).
// The order is the contract: the guard reads the panic and fault records, the report takes them
// (ListRules GuardBeforeReport).
//
// Options names any member it changes (each optional, as a type member):
//     struct MyRuntime {
//         using FaultHandler   = Kvasir::Fault::HandlerFor<Kvasir::Fault::Options{.reserveBytes = 2048}>;
//         using StackProtector = Kvasir::StackProtector<64>;
//         using GuardConfig    = MyGuardConfig;   // BootGuard's Config; void: no boot guard
//         using Report         = Kvasir::Boot::Report<Kvasir::Boot::ParsedLine>;   // or Boot::ReportInMain
//     };
//     Kvasir::Runtime::Standard<Clock, MyRuntime>
// The defaults: Fault::HandlerFor<>, StackProtector<>, StandardGuardConfig, Boot::Report<>. What the guard does at its
// limit is the image's BootGuard::LimitReaction (default OnLimit::Log: its line).
#include "kvasir/Mpl/brigand.hpp"
#include "kvasir/Runtime/BootGuard.hpp"
#include "kvasir/Runtime/BootReport.hpp"
#include "kvasir/Runtime/FaultHandler.hpp"
#include "kvasir/Runtime/StackProtector.hpp"
#include "kvasir/Util/Config.hpp"
#include "kvasir/Util/Periodic.hpp"

#include <chrono>
#include <cstdint>
#include <type_traits>

namespace Kvasir::Runtime {
/// Standard's boot guard: the policy at the third failed run in a row; a run of 30 s is healthy.
struct StandardGuardConfig {
    static constexpr std::uint16_t limit        = 3;
    static constexpr auto          healthyAfter = std::chrono::seconds{30};
};

/// Standard<Clock> without options.
struct StandardDefaults {};

namespace Detail {
    template<typename O>
    using FaultHandlerOf = typename O::FaultHandler;
    template<typename O>
    using StackProtectorOf = typename O::StackProtector;
    template<typename O>
    using GuardConfigOf = typename O::GuardConfig;
    template<typename O>
    using ReportOf = typename O::Report;

    template<typename GuardConfig, typename Clock>
    struct GuardEntry {
        using type = brigand::list<BootGuard::Guard<GuardConfig, Clock>>;
    };

    template<typename Clock>
    struct GuardEntry<void, Clock> {
        using type = brigand::list<>;
    };
}   // namespace Detail

template<PollClock Clock, typename Options = StandardDefaults>
struct Standard {
    using FaultHandler
      = ::Kvasir::Config::TypeOr<Options, Fault::HandlerFor<>, Detail::FaultHandlerOf>;
    using StackProtector
      = ::Kvasir::Config::TypeOr<Options, Kvasir::StackProtector<>, Detail::StackProtectorOf>;
    using GuardConfig
      = ::Kvasir::Config::TypeOr<Options, StandardGuardConfig, Detail::GuardConfigOf>;
    using Report = ::Kvasir::Config::TypeOr<Options, Boot::Report<>, Detail::ReportOf>;

    // the boot guard it lists (Guard::report(), markHealthy(), plannedReset() ...); void without one
    using Guard = brigand::front<
      brigand::append<typename Detail::GuardEntry<GuardConfig, Clock>::type, brigand::list<void>>>;

    // the order is the contract: the guard reads the records, the report takes them
    using StartupEntries = brigand::append<brigand::list<FaultHandler, StackProtector>,
                                           typename Detail::GuardEntry<GuardConfig, Clock>::type,
                                           brigand::list<Report>>;
};
}   // namespace Kvasir::Runtime
