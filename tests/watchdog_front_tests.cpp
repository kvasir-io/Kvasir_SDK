// Kvasir::Health::WatchdogFront (Health/WatchdogFront.hpp) alone, without a chip or the supervisor: which of
// arm/feed/disarm exist for a gated and an ungated watchdog, with and without a disarmNow() body, that each calls its
// private body exactly once, gatedFeedOf<Config>(), and that the front satisfies WatchdogShape. The FeedKey comes
// from a Supervisor specialisation of this test's own (HealthKey.hpp only declares the template; its friend
// declaration covers every specialisation), so nothing of Supervisor.hpp is compiled.
#include "kvasir/Health/HealthKey.hpp"
#include "kvasir/Health/WatchdogFront.hpp"
#include "test_harness.hpp"

#include <chrono>
#include <type_traits>

namespace watchdog_front_test {
struct KeyTag {};
}   // namespace watchdog_front_test

template<>
struct Kvasir::Health::Supervisor<watchdog_front_test::KeyTag, void, void, void> {
    static Kvasir::Health::FeedKey key() { return {}; }
};

namespace watchdog_front_test {
using Kvasir::Health::FeedKey;
using Kvasir::Health::gatedFeedOf;
using Kvasir::Health::WatchdogFront;

FeedKey key() { return Kvasir::Health::Supervisor<KeyTag, void, void, void>::key(); }

struct Calls {
    int arm{};
    int feed{};
    int disarm{};
};

// A chip watchdog in the shape the header comment gives: bodies private, the front a friend.
template<bool Gated, bool CanStop>
struct Fake : WatchdogFront<Fake<Gated, CanStop>, Gated> {
    static constexpr auto timeout = std::chrono::milliseconds{100};
    static inline Calls   calls{};

private:
    friend struct WatchdogFront<Fake, Gated>;

    static void armNow() { ++calls.arm; }

    static void feedNow() { ++calls.feed; }

    static void disarmNow()
        requires(CanStop)
    {
        ++calls.disarm;
    }
};

using Plain       = Fake<false, true>;
using Gated       = Fake<true, true>;
using PlainNoStop = Fake<false, false>;
using GatedNoStop = Fake<true, false>;

// which forms exist
template<typename W>
concept PlainArm = requires { W::arm(); };
template<typename W>
concept PlainFeed = requires { W::feed(); };
template<typename W>
concept PlainDisarm = requires { W::disarm(); };
template<typename W>
concept KeyArm = requires(FeedKey k) { W::arm(k); };
template<typename W>
concept KeyFeed = requires(FeedKey k) { W::feed(k); };
template<typename W>
concept KeyDisarm = requires(FeedKey k) { W::disarm(k); };
template<typename W>
concept BodyVisible = requires { W::feedNow(); };

static_assert(PlainArm<Plain> && PlainFeed<Plain> && PlainDisarm<Plain>);
static_assert(!KeyArm<Plain> && !KeyFeed<Plain> && !KeyDisarm<Plain>);
static_assert(KeyArm<Gated> && KeyFeed<Gated> && KeyDisarm<Gated>);
static_assert(!PlainArm<Gated> && !PlainFeed<Gated> && !PlainDisarm<Gated>,
              "a gated watchdog takes the key");
static_assert(PlainArm<PlainNoStop> && PlainFeed<PlainNoStop> && !PlainDisarm<PlainNoStop>,
              "no body, no disarm");
static_assert(KeyArm<GatedNoStop> && KeyFeed<GatedNoStop> && !KeyDisarm<GatedNoStop>);
static_assert(!KeyDisarm<PlainNoStop> && !PlainDisarm<GatedNoStop>);
static_assert(!BodyVisible<Plain> && !BodyVisible<Gated>,
              "the bodies stay private");
static_assert(!std::is_default_constructible_v<FeedKey>,
              "only a Supervisor makes a key");

static_assert(Kvasir::Health::WatchdogShape<Plain> && Kvasir::Health::WatchdogShape<Gated>
              && Kvasir::Health::WatchdogShape<GatedNoStop>);

struct NoKnob {};

struct GatedConfig {
    static constexpr bool gatedFeed = true;
};

struct UngatedConfig {
    static constexpr bool gatedFeed = false;
};

static_assert(!gatedFeedOf<NoKnob>() && gatedFeedOf<GatedConfig>()
              && !gatedFeedOf<UngatedConfig>());
}   // namespace watchdog_front_test

using namespace watchdog_front_test;

int main() {
    using Kvasir::Test::test;
    {
        test("ungated: arm, feed, disarm each call their body once");
        Plain::arm();
        Plain::feed();
        Plain::feed();
        Plain::disarm();
        CHECK(Plain::calls.arm == 1 && Plain::calls.feed == 2 && Plain::calls.disarm == 1);
    }
    {
        test("gated: the keyed forms call the same bodies");
        Gated::arm(key());
        Gated::feed(key());
        Gated::disarm(key());
        CHECK(Gated::calls.arm == 1 && Gated::calls.feed == 1 && Gated::calls.disarm == 1);
    }
    {
        test("without disarmNow: arm and feed only, nothing stops it");
        GatedNoStop::arm(key());
        GatedNoStop::feed(key());
        PlainNoStop::feed();
        CHECK(GatedNoStop::calls.arm == 1 && GatedNoStop::calls.feed == 1
              && GatedNoStop::calls.disarm == 0);
        CHECK(PlainNoStop::calls.feed == 1 && PlainNoStop::calls.disarm == 0);
    }
    return Kvasir::Test::report();
}
