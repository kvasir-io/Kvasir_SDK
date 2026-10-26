#pragma once
// The std::chrono shell of a hardware clock (the cores' SysTick clock, the RP TIMER clock): the `duration`, `rep`,
// `period`, `time_point` and `is_steady` a Clock requirement asks for, and `time_point +/- any duration` (converted to
// the clock's own duration first, so `now() + 5ms` is the clock's time_point). The clock derives from it and adds
// `now()`:
//
//   struct MyClock : Kvasir::Chrono::SteadyClock<MyClock, std::chrono::duration<std::int64_t, std::micro>> {
//       static time_point now();
//   };
//
// A class template that derives from it names the types with `using typename Base::duration;` (a dependent base's
// members are not found unqualified). The two operators are hidden friends: found by argument-dependent lookup
// through the time_point's clock, which derives from this class.
#include <chrono>
#include <type_traits>

namespace Kvasir::Chrono {
template<typename Derived, typename Duration>
struct SteadyClock {
    using duration   = Duration;
    using rep        = typename duration::rep;
    using period     = typename duration::period;
    using time_point = std::chrono::time_point<Derived, duration>;

    static constexpr bool is_steady = true;

    template<typename Rep,
             typename Period>
        requires(!std::is_same_v<std::chrono::duration<Rep,
                                                       Period>,
                                 duration>)
    friend constexpr time_point operator+(time_point                    t,
                                          std::chrono::duration<Rep,
                                                                Period> d) {
        return t + std::chrono::duration_cast<duration>(d);
    }

    template<typename Rep,
             typename Period>
        requires(!std::is_same_v<std::chrono::duration<Rep,
                                                       Period>,
                                 duration>)
    friend constexpr time_point operator-(time_point                    t,
                                          std::chrono::duration<Rep,
                                                                Period> d) {
        return t - std::chrono::duration_cast<duration>(d);
    }
};
}   // namespace Kvasir::Chrono
