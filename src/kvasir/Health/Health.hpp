#pragma once
// A health-gated watchdog: subsystems declare a check next to their other Startup hooks and check in when they make
// progress; the supervisor feeds the watchdog only while every check is inside its window. A check that falls
// silent is named - in the log, and in a CRC-checked .noInit record the next boot reports.
//
//     struct I2cHealth {
//         static constexpr std::string_view name = "i2c1";
//         static constexpr auto maxInterval      = std::chrono::milliseconds{200};   // silent longer: starved
//     };
//     struct I2cEngine {
//         static void turn();   // if(progressed) Kvasir::Health::checkIn<I2cHealth>();
//         using Extends = brigand::list<Kvasir::Startup::Extend<Kvasir::Hook::MainLoop, &turn>,
//                                       Kvasir::Health::Check<I2cHealth>>;
//     };
//
//     using Startup = Kvasir::Startup::Startup<HW::ClockSettings, ..., Kvasir::Boot::Report<>, I2cEngine,
//                                              Kvasir::Health::Service<>>;   // the boot line before main
//     using Health  = Kvasir::Health::Supervisor<Startup, HW::Watchdog, HW::SystickClock, HealthConfig>;
//     template<>
//     struct Kvasir::Health::SupervisorFor<Kvasir::Tag::User> { using type = ::Health; };   // Service<>'s supervisor
//     int main() {
//         while(true) { Startup::run<Kvasir::Hook::MainLoop>(); }   // Service<> calls Health::turn() last
//     }
//
// Health::Service<> is the supervisor as a Startup entry (Health/Supervisor.hpp, why the binding line exists). Its
// runtimeInit is the boot half, before main and after the boot report (ListRules HealthServiceAfterReport):
// Health::holdOff() - a watchdog the last run left armed waits for the first turn - and Health::logReport() - the
// check that starved last time. The hand form - no Service<> in the list, `Health::service()` (or `Health::turn()`)
// after run<MainLoop>() in the loop - calls both itself, first in main:
//
//     int main() {
//         Health::holdOff();
//         Health::logReport();
//         while(true) { Startup::run<Kvasir::Hook::MainLoop>(); Health::service(); }
//     }
//
// One form or the other, never both.
//
// The supervisor owns the watchdog: its first service() arms it (a watchdog with arm(); one without is fed), from
// then on only service() feeds, and holdOff() - before that first turn only - stops one that is already running.
// What it needs of the watchdog is Health::WatchdogShape (Health/HealthKey.hpp): feed() and a Timeout duration.
//
// Check Config: name, maxInterval (required), graceAfterBoot (default maxInterval: the first window is grace +
// maxInterval), startSuspended (false; Supervisor::resume<C>() starts it). Supervisor Config: period (10 ms),
// statistics (true: Supervisor::worstMs(i) keeps each check's longest silence).
//
// Reactions on a starvation, the image's choice through the late hook StarvationReaction (`template<> struct
// Kvasir::Health::StarvationReaction<Kvasir::Tag::User> { using type = ...; };`, the table in kvasir/Port/Hooks.hpp):
// OnStarvation::StopFeeding (the default: log, record, announce the coming reset to a printer, let the watchdog bite),
// OnStarvation::Panic (Panic::Cause::healthCheck, the check's index as the detail: the application's panic handler
// makes it safe), OnStarvation::Log (bench: log, keep feeding), OnStarvation::Count (count, keep feeding),
// OnStarvation::Call<&fn, keepFeeding> (the application's function; the watchdog resets after it unless keepFeeding). A
// supervised image should set its watchdog's `gatedFeed`, so only the supervisor can feed, arm or disarm it
// (Health/HealthKey.hpp).
//
// A local recovery before the policy: a check Config may have `onStarved`, in one of four forms - `void()`,
// `Recovery()`, `void(Attempt)`, `Recovery(Attempt)` (void counts as Recovery::started). When the check starves, the
// supervisor calls it instead of the policy, gives the check `recoveryWindow` (default maxInterval) to beat again
// and goes on feeding the watchdog meanwhile; it tries `recoveryAttempts` times (default 1, at most 15), a beat
// starts the ladder over, and only a check still silent after the last rung (or one whose onStarved returns
// Recovery::cannot) goes to the policy. Each recovery is logged and kept in the "KHRV" record, which logReport()
// reports at the next boot - also a reset that happened inside onStarved.
//
//     struct BusHealth {
//         static constexpr std::string_view name = "i2c";
//         static constexpr auto maxInterval      = std::chrono::seconds{10};
//         static constexpr auto recoveryAttempts = 2;
//         static constexpr auto recoveryWindow   = std::chrono::seconds{3};
//         static Kvasir::Health::Recovery onStarved(Kvasir::Health::Attempt a);   // a.n = 0 .. a.of - 1
//     };
//
// The contract of onStarved: it runs in thread mode, from service() (on core 0 for a core 1 check too). It STARTS a
// recovery and returns - a request, a flag the owning loop serves, a peripheral reset with a bounded wait - and never
// waits for the subsystem to come back. It does not reset the chip: return Recovery::cannot to escalate, so the
// record and the printer's announcement happen. The time it takes is measured against the Supervisor Config's
// `recoveryBudget` (2 ms): over it, a warning and a count (Supervisor::recoveryOverruns()). A hang inside it is the
// watchdog's, and the next boot names the check and the rung.
//
// Deferred recoveries (deferRecoveries) and the timing bound of the ladder: Health/Ladder.hpp.
//
// The parts: Health/Check.hpp (what a driver that declares a check includes, Hooks + std only), Health/Policies.hpp,
// Health/Ladder.hpp, Health/Supervisor.hpp. This header includes them all.
#include "kvasir/Health/Check.hpp"
#include "kvasir/Health/Policies.hpp"
#include "kvasir/Health/Supervisor.hpp"
