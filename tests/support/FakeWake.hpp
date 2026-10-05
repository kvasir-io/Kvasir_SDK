#pragma once

#include <optional>

/// Kvasir::Executor's Wake and Sleep on the host: FakeWake records the armed time, HostSleep "sleeps" by moving the
/// fake clock to it - a tickless sleep in one assignment.
namespace Kvasir::Test {

template<typename Clock>
struct FakeWake {
    static inline std::optional<typename Clock::time_point> armedAt{};
    static inline int                                       arms{};

    static void arm(typename Clock::time_point t) {
        armedAt = t;
        ++arms;
    }

    static void disarm() { armedAt.reset(); }
};

template<typename Clock>
struct HostSleep {
    static constexpr bool sleeps = true;
    static inline int     slept{};

    template<typename F>
    static void until(F&& stillIdle) {
        if(stillIdle() && FakeWake<Clock>::armedAt) {
            Clock::current = *FakeWake<Clock>::armedAt;
            ++slept;
        }
    }
};
}   // namespace Kvasir::Test
