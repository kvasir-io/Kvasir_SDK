#pragma once
// Where a driver is with the part (or the peer) it talks to, on any bus, and the two pieces most drivers count it with:
//
//   FailureStreak<N>   failures in a row, saturating; "absent" from N on. For a driver that derives its link from its
//                      own state: `streak_.link(up)`.
//   LinkTracker<N>     a stored link with a bring-up count: answering on success, absent after N failures in a row;
//                      a failure while answering keeps it answering until N.
#include <cstdint>

namespace Kvasir {

enum class Link : std::uint8_t {
    starting,    ///< nothing acknowledged since the driver (re)started: in bring-up, or waiting
                 ///< for the first answer of a part that has no bring-up script
    answering,   ///< through its bring-up, and the part has answered since
    absent,      ///< parked after failures in a row, and probed now and then
    offline,     ///< deliberately not talked to (its path is switched off): neither a failure
                 ///< nor an absence
};

/// Failures in a row, saturating at 255; absent from `AbsentAfter` on.
template<std::uint8_t AbsentAfter>
struct FailureStreak {
    static_assert(AbsentAfter != 0,
                  "absent after at least one failure");

    std::uint8_t n{};

    constexpr void failed() {
        if(n != 0xFF) { ++n; }
    }

    constexpr void cleared() { n = 0; }

    [[nodiscard]] constexpr bool absent() const { return n >= AbsentAfter; }

    /// Exactly at the threshold: true once per streak, for a log line.
    [[nodiscard]] constexpr bool justAbsent() const { return n == AbsentAfter; }

    /// The usual derivation, `up` being the driver's own "it answers".
    [[nodiscard]] constexpr Link link(bool up) const {
        if(up) { return Link::answering; }
        return absent() ? Link::absent : Link::starting;
    }
};

/// A stored link and the number of bring-ups.
template<std::uint8_t AbsentAfter, typename BringUps = std::uint16_t>
class LinkTracker {
public:
    constexpr void up() {
        if(link_ != Link::answering) { ++bringUps_; }
        link_ = Link::answering;
        streak_.cleared();
    }

    constexpr void failed() {
        streak_.failed();
        if(streak_.absent()) { link_ = Link::absent; }
    }

    [[nodiscard]] constexpr Link link() const { return link_; }

    [[nodiscard]] constexpr BringUps bringUps() const { return bringUps_; }

    [[nodiscard]] constexpr std::uint8_t failures() const { return streak_.n; }

private:
    Link                       link_{Link::starting};
    FailureStreak<AbsentAfter> streak_{};
    BringUps                   bringUps_{};
};
}   // namespace Kvasir
