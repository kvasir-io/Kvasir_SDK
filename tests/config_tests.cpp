// KVASIR_CONFIG_OR and Kvasir::Config::TypeOr (kvasir/Util/Config.hpp): every check is a static_assert, the run only
// reports.
#include "kvasir/Util/Config.hpp"

#include "test_harness.hpp"

#include <chrono>
#include <cstdint>
#include <type_traits>

namespace {
using namespace std::chrono_literals;

struct NoWake {};

template<typename C>
using WakeOf = typename C::Wake;

template<typename Config>
struct Component {
    // A plain
    static constexpr bool statistics = KVASIR_CONFIG_OR(Config, statistics, true);
    // B cast: the cast stays outside, the declared type stays
    static constexpr std::uint8_t retries
      = static_cast<std::uint8_t>(KVASIR_CONFIG_OR(Config, retries, 3));
    // B brace: braces keep the narrowing check
    static constexpr std::uint32_t maxPerTurn
      = std::uint32_t{KVASIR_CONFIG_OR(Config, maxPerTurn, std::uint32_t{8})};
    // C duration: converted at the use
    static constexpr std::chrono::microseconds period
      = std::chrono::duration_cast<std::chrono::microseconds>(
        KVASIR_CONFIG_OR(Config, period, 10ms));
    // a fallback with commas
    static constexpr bool sameType = KVASIR_CONFIG_OR(Config, sameType, std::is_same_v<int, long>);
    // a fallback naming another member
    static constexpr auto grace = KVASIR_CONFIG_OR(Config, grace, period * 2);
    // a type knob
    using Wake = ::Kvasir::Config::TypeOr<Config, NoWake, WakeOf>;

    template<typename C>
    static constexpr bool statisticsOf() {
        return KVASIR_CONFIG_OR(C, statistics, false);
    }
};

struct Empty {};

struct Set {
    static constexpr bool     statistics = false;
    static constexpr int      retries    = 7;
    static constexpr unsigned maxPerTurn = 3;
    static constexpr auto     period     = 2s;
    static constexpr bool     sameType   = true;

    struct Wake {};
};

struct Defaults {
    static constexpr unsigned maxPerTurn = 5;
};

struct Derived : Defaults {
    static constexpr bool statistics = false;
};

using E = Component<Empty>;
using S = Component<Set>;
using D = Component<Derived>;

static_assert(E::statistics && E::retries == 3 && E::maxPerTurn == 8 && E::period == 10ms
              && !E::sameType);
static_assert(E::grace == 20ms
              && std::is_same_v<E::Wake,
                                NoWake>);
static_assert(!S::statistics && S::retries == 7 && S::maxPerTurn == 3 && S::period == 2s
              && S::sameType);
static_assert(S::grace == 4s
              && std::is_same_v<S::Wake,
                                Set::Wake>);
static_assert(!D::statistics && D::maxPerTurn == 5,
              "a member from the defaults a config derives from");
static_assert(!E::statisticsOf<Set>() && !E::statisticsOf<Derived>() && !S::statisticsOf<Empty>());

// the deduced types are the member's or the fallback's, as the hand-written lambda had them
template<typename C>
using RawPeriod = decltype(KVASIR_CONFIG_OR(C, period, 10ms));
static_assert(std::is_same_v<RawPeriod<Empty>,
                             std::chrono::milliseconds>);
static_assert(std::is_same_v<RawPeriod<Set>,
                             std::chrono::seconds>);
template<typename C>
using RawRetries = decltype(KVASIR_CONFIG_OR(C, retries, 3U));
static_assert(std::is_same_v<RawRetries<Empty>,
                             unsigned>
              && std::is_same_v<RawRetries<Set>,
                                int>);
}   // namespace

int main() {
    Kvasir::Test::test("config knobs");
    CHECK(E::statistics);
    CHECK(!S::statistics);
    return Kvasir::Test::report();
}
