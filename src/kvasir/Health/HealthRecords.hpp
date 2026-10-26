#pragma once
// The records a health supervisor (Health/Health.hpp) leaves for the next boot, apart from the supervisor so that the
// boot-loop guard (Runtime/BootGuard.hpp) can read them without the supervisor's templates: "KHLT", the starvation
// that ended the run, and "KHRV", the recoveries by a check's onStarved (and the one a reset cut short).
#include "kvasir/Util/Persistent.hpp"
#include "kvasir/Util/attributes.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

namespace Kvasir::Health {
// The record of a starvation, Persistent "KHLT".
struct Starvation {
    std::uint32_t check;      // index in the supervisor's list
    std::uint32_t nameHash;   // FNV-1a of the name: the next build may order the checks differently
    std::uint32_t silentMs;
    std::uint32_t allowedMs;
    std::uint32_t uptimeMs;
    std::uint32_t silentMask;   // the checks past half their window at that moment
};

struct StarvationTag {
    static constexpr std::uint32_t id = persistentId("KHLT");
};

[[KVASIR_NOINIT]] inline Persistent<Starvation, StarvationTag> lastStarvation;

/// Whether a starvation record is there, without taking it (a boot guard).
inline bool starvationRecorded() { return lastStarvation.valid(); }

// The recoveries of a run, Persistent "KHRV". Written only by a supervisor with at least one onStarved.
struct Recoveries {
    std::uint32_t total;          // recoveries that ended in a beat, this run
    std::uint32_t lastCheck;      // index of the last recovered check
    std::uint32_t lastNameHash;   // FNV-1a, as Starvation::nameHash
    std::uint32_t lastUptimeMs;
    // 0: no onStarved running; else (check + 1) | (attempt << 16), attempt counted from 1, set while one runs: a
    // reset inside it is named at the next boot
    std::uint32_t inProgress;
    // 0: no escalation; else the rungs spent before the policy ran
    std::uint32_t escalatedAttempts;
};

struct RecoveriesTag {
    static constexpr std::uint32_t id = persistentId("KHRV");
};

[[KVASIR_NOINIT]] inline Persistent<Recoveries, RecoveriesTag> lastRecovery;

constexpr std::uint32_t recoveryInProgress(std::uint32_t check,
                                           std::uint32_t attempt) {
    return (check + 1) | (attempt << 16U);
}

constexpr std::uint32_t inProgressCheck(std::uint32_t inProgress) {
    return (inProgress & 0xFFFFU) - 1;
}

constexpr std::uint32_t inProgressAttempt(std::uint32_t inProgress) { return inProgress >> 16U; }

/// The check the run before this one starved, without taking a record: from a starvation record, else from a
/// recovery a reset cut short (the watchdog bit inside onStarved). For a boot guard.
inline std::optional<std::uint32_t> starvedCheck() {
    if(auto const s = lastStarvation.load()) { return s->check; }
    if(auto const r = lastRecovery.load(); r && r->inProgress != 0) {
        return inProgressCheck(r->inProgress);
    }
    return std::nullopt;
}

constexpr std::uint32_t fnv1a(std::string_view s) {
    std::uint32_t h = 0x811C'9DC5U;
    for(char const c : s) { h = (h ^ static_cast<std::uint8_t>(c)) * 0x0100'0193U; }
    return h;
}
}   // namespace Kvasir::Health
