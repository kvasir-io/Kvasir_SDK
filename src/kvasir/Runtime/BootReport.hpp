#pragma once
// The boot report as a Startup entry: the version, the reset cause and the fault or panic the run before this one
// ended in, as the first lines of the image - from the entry's runtimeInit, before main:
//
//     using Startup = Kvasir::Startup::Startup<HW::ClockSettings, Clock, HW::ComBackend, Kvasir::Boot::Report<>, ...>;
//
// List it right after the ComBackend (its lines need the log) and after a BootGuard::Guard (the guard reads the
// records this takes; the list rule GuardBeforeReport refuses the other order). Runtime::Standard lists it, with the
// fault handler, the stack protector and a boot guard before it. The forms:
//
//   Report<>             Boot::logBoot(<the reset cause>): "boot: <version> reset cause: <cause>", then what the run
//                        before this one ended in
//   Report<ParsedLine>   "Reset cause: <cause>", then what the run before ended in: for a firmware whose host scripts
//                        begin a boot at that line (no version in it: main logs its own)
//   ReportInMain         no lines: for a main that reports by hand on purpose, because it reads the records itself
//                        first (Boot::takePrevious, Panic::takeLastPanic) and then logs them
//
// A list with the user log backend names one of the three (ListRules BootReportListed). The reset cause is the chip's,
// through the late hook Port::ResetCause<Tag::User> (kvasir/Port/Hooks.hpp; the chip's PM::resetCause()), so this
// header may come before the chip's.
#include "kvasir/Port/Hooks.hpp"
#include "kvasir/Runtime/BootLog.hpp"

#include <type_traits>

namespace Kvasir::Boot {
/// Report<>'s form: logBoot's lines.
struct Plain {};

/// Report<ParsedLine>'s form: a "Reset cause: <cause>" line that host scripts parse, then the records.
struct ParsedLine {};

template<typename Form = Plain>
struct Report {
    static_assert(std::is_same_v<Form,
                                 Plain>
                    || std::is_same_v<Form,
                                      ParsedLine>,
                  "Boot::Report<Form>: Form is Boot::Plain or Boot::ParsedLine");

    static constexpr bool isBootReport = true;

    static void runtimeInit() {
        using Cause = Port::LateT<Port::ResetCause<Tag::User>, Form>;
        static_assert(
          requires { sizeof(Cause); },
          "Kvasir port: no chip package specialised Port::ResetCause<Tag::User> "
          "(kvasir/Port/Hooks.hpp) "
          "in this translation unit: include the chip's headers (chip/chip.hpp)");
        if constexpr(std::is_same_v<Form, ParsedLine>) {
            auto const cause    = Cause{}();
            auto const previous = takePrevious();
            // DEBUG after a routine boot, WARN after a fault or panic (as logBoot's line)
            if(endedInFailure(previous)) {
                UC_LOG_W("Reset cause: {}", cause);
            } else {
                UC_LOG_D("Reset cause: {}", cause);
            }
            logStartKind(cause);
            logPrevious(previous);
        } else {
            logBoot(Cause{}());
        }
    }
};

/// For a main that reports by hand on purpose: it reads the records itself first (Boot::takePrevious,
/// Panic::takeLastPanic, Fault::takeLastFault) and logs them. A check-only entry: no init step, no hook.
struct ReportInMain {
    static constexpr bool isStartupEntry   = true;
    static constexpr bool bootReportInMain = true;
};
}   // namespace Kvasir::Boot
