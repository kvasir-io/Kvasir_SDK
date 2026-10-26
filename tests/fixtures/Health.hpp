// The Kvasir::Health::Supervisor scenario fixture of the SDK's own health tests (health_scenarios_tests.cpp,
// health_recovery_tests.cpp; not meant for other repositories).
//
// A supervisor's state is static, so every scenario has its own instantiation: a tag type (a local `struct T {}`)
// on its checks, its Startup and its watchdog. The policy is the program's: each test program specialises
//
//     template<>
//     struct Kvasir::Health::StarvationReaction<Kvasir::Tag::User> { using type = TestPolicy; };
//
// once, in its own TU (a late specialisation wins), and reads what it saw through Seen / Calls. The drivers move
// only the fake clock: a turn is `advance(10ms); S::service();`.
#pragma once

#include "../support/FakeClock.hpp"
#include "kvasir/Health/Health.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>

namespace Kvasir::Test::HealthFixture {
using namespace std::chrono_literals;

using FakeClock = Kvasir::Test::FakeClockT<std::chrono::milliseconds>;
// the hand-written clocks these tests had counted in std::int64_t milliseconds
static_assert(std::is_same_v<FakeClock::rep,
                             std::int64_t>
              || sizeof(FakeClock::rep) == 8);

inline void advance(std::chrono::milliseconds d) { FakeClock::advance(d); }

// what the policy saw, for every scenario of the program
struct Seen {
    static inline std::vector<Kvasir::Health::Starved> all;
};

// the policy the programs install: records, stops feeding
struct TestPolicy {
    static constexpr bool keepFeeding = false;

    static void operator()(Kvasir::Health::Starved const& s) { Seen::all.push_back(s); }
};

template<typename Tag>
struct Watchdog {
    static constexpr auto timeout = std::chrono::milliseconds{500};
    // not every scenario reads it
    [[maybe_unused]] static inline int feeds = 0;

    static void feed() { ++feeds; }
};

// a gated watchdog, as chip_rp_common's with Config::gatedFeed: no plain feed()
template<typename Tag>
struct GatedWatchdog {
    static constexpr auto timeout = std::chrono::milliseconds{500};
    // not every scenario reads it
    [[maybe_unused]] static inline int feeds = 0;

    static void feed(Kvasir::Health::FeedKey) { ++feeds; }
};

// how often the AllChecksReported hook ran
template<typename Tag>
[[maybe_unused]] inline int allReported = 0;

template<typename Tag, typename... Cs>
struct Startup {
    template<typename Hook>
    using ExtendsOn = brigand::list<Kvasir::Health::Check<Cs>...>;

    template<typename Hook>
    static void run() {
        ++allReported<Tag>;
    }
};

struct Period10 {
    static constexpr auto period = 10ms;
};

template<typename Tag, typename... Cs>
using Sup = Kvasir::Health::Supervisor<Startup<Tag, Cs...>, Watchdog<Tag>, FakeClock, Period10>;

// the policy calls a scenario caused
struct Calls {
    std::size_t before = Seen::all.size();

    [[nodiscard]] std::size_t count() const { return Seen::all.size() - before; }

    [[nodiscard]] Kvasir::Health::Starved const& last() const { return Seen::all.back(); }
};

// ---- turn drivers: a turn is 10 ms of the fake clock, then S::service() --------------------------------------------

// n turns, nobody beats
template<typename S>
void turns(int n) {
    for(int i = 0; i < n; ++i) {
        advance(10ms);
        S::service();
    }
}

// n turns, the checks Beats... beat in each (before service(), as the MainLoop hooks run)
template<typename S,
         typename... Beats>
void turnsBeating(int n) {
    for(int i = 0; i < n; ++i) {
        advance(10ms);
        (Kvasir::Health::checkIn<Beats>(), ...);
        S::service();
    }
}

// the start of most scenarios: the first turn, the checks Beats... beat, one turn later the steady window
template<typename S,
         typename... Beats>
void beatOnce() {
    S::service();
    (Kvasir::Health::checkIn<Beats>(), ...);
    advance(10ms);
    S::service();
}

// silent turns until p() holds: the turn it held after (1-based), or -1 if not within max turns
template<typename S,
         typename Pred>
int turnsUntil(int  max,
               Pred p) {
    for(int i = 1; i <= max; ++i) {
        advance(10ms);
        S::service();
        if(p()) { return i; }
    }
    return -1;
}
}   // namespace Kvasir::Test::HealthFixture
