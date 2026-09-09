#pragma once

#include <chrono>

/// A clock a test winds by hand, so mandated waits take no time and every run is the
/// same. `FakeClock` counts microseconds (what the device drivers' clocks do); a test
/// that thinks in another unit names it: `FakeClockT<std::chrono::milliseconds>`.
namespace Kvasir::Test {

template<typename Duration = std::chrono::microseconds>
struct FakeClockT {
    using duration   = Duration;
    using rep        = typename duration::rep;
    using period     = typename duration::period;
    using time_point = std::chrono::time_point<FakeClockT, duration>;

    static inline time_point current{};

    static time_point now() { return current; }

    static void reset() { current = time_point{}; }

    template<typename Rep,
             typename Period>
    static void advance(std::chrono::duration<Rep,
                                              Period> by) {
        current += std::chrono::duration_cast<duration>(by);
    }

    /// The clock set to `t` after its epoch.
    template<typename Rep,
             typename Period>
    static void set(std::chrono::duration<Rep,
                                          Period> t) {
        current = time_point{} + std::chrono::duration_cast<duration>(t);
    }
};

using FakeClock = FakeClockT<>;

}   // namespace Kvasir::Test
