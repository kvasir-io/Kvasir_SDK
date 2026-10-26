#pragma once
// What a driver that declares a health check includes, and nothing more: the check's Config concept, Check<C> for
// its Startup list, checkIn<C>(), and the onStarved vocabulary (Recovery, Attempt). The supervisor, its policies and
// the recovery ladder are in the umbrella kvasir/Health/Health.hpp (its comment is the user guide).
#include "kvasir/Port/Extends.hpp"

#include <chrono>
#include <cstdint>
#include <string_view>

namespace Kvasir::Health {
template<typename C>
concept CheckConfig = requires {
    { std::string_view{C::name} };
    { std::chrono::duration_cast<std::chrono::milliseconds>(C::maxInterval) };
};

template<CheckConfig C>
struct Check {
    using Hook = Kvasir::Hook::HealthCheck;

    static void noop() {}

    static constexpr auto fn = &noop;
    using config             = C;
    // Written by the checking context only; read by the supervisor, maybe on the other core. An aligned word store
    // is single-copy atomic (Armv6-M DDI0419E A3.5.1, Armv8-M DDI0553B.y B7.2.1): no lock, the supervisor only
    // asks "did it change".
    static inline std::uint32_t volatile beats = 0;
};

/// Progress: one load and one store.
template<CheckConfig C>
[[gnu::always_inline]] inline void checkIn() {
    Check<C>::beats = Check<C>::beats + 1;
}

// A check's local recovery (onStarved), see the header comment.
enum class Recovery : std::uint8_t {
    started,   // a recovery is under way: the check gets a fresh window to beat again
    cannot,    // no lever left: escalate to the policy now
};

struct Attempt {
    std::uint8_t n;    // 0 .. of - 1: the rung of the ladder
    std::uint8_t of;   // the check's recoveryAttempts
};

template<typename C>
concept PlainRecovery = requires { C::onStarved(); };
template<typename C>
concept LadderRecovery = requires(Attempt a) { C::onStarved(a); };
template<typename C>
concept Recovers = PlainRecovery<C> || LadderRecovery<C>;
}   // namespace Kvasir::Health
