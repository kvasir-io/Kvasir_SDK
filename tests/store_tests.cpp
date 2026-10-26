// Kvasir::Flash::RetryingStore / CachedValue (Runtime/Store.hpp): a fake store whose flash write fails N times - pending,
// the 1/2/4 s schedule from a fake clock, the log hooks, recovery, the link. Off the real clock.
#include "kvasir/Runtime/Store.hpp"

#include "support/FakeClock.hpp"
#include "test_harness.hpp"

#include <chrono>
#include <cstdint>
#include <type_traits>

namespace store_test {
using TestClock = Kvasir::Test::FakeClockT<std::chrono::milliseconds>;
using ms        = std::chrono::milliseconds;

enum class E : std::uint8_t { refused, verify };

// the "flash": one value, a write fails while failsLeft > 0, and drops (writes nothing, says ok) while dropsLeft > 0
template<typename Tag>
struct FakeFlash {
    static inline std::uint32_t stored{};
    static inline int           failsLeft{};
    static inline int           dropsLeft{};
    static inline int           writes{};
};

template<typename Clock>
struct Fake : Kvasir::Flash::CachedValue<Fake<Clock>, std::uint32_t, Clock, E> {
    using F = FakeFlash<Clock>;
    static inline int failedLogs{};
    static inline int recoveredLogs{};
    static inline E   lastLogged{};

    static std::uint32_t readFlashValue() { return F::stored; }

    static Kvasir::Result<void,
                          E>
    internalWrite() {
        ++F::writes;
        if(F::failsLeft > 0) {
            --F::failsLeft;
            return std::unexpected{E::refused};
        }
        if(F::dropsLeft > 0) {
            --F::dropsLeft;
            return {};
        }
        F::stored = Fake::ramCopy;
        return {};
    }

    template<typename R,
             typename D>
    static void logFailed(R const&,
                          E e,
                          D) {
        ++failedLogs;
        lastLogged = e;
    }

    template<typename R>
    static void logRecovered(R const&) {
        ++recoveredLogs;
    }
};

using S = Fake<TestClock>;

static_assert(Kvasir::Flash::CachedStore<S>);
static_assert(!std::is_same_v<S::Extends,
                              brigand::list<>>,
              "turn() on Hook::MainLoop");
}   // namespace store_test

using namespace store_test;

int main() {
    using Kvasir::Test::test;
    using F = FakeFlash<TestClock>;
    TestClock::reset();

    {
        test("first value(): read from the flash once");
        F::stored = 7;
        CHECK(S::value() == 7);
        F::stored = 9;
        CHECK(S::value() == 7);
        F::stored = 7;
    }

    {
        test("equal to the flash: true, nothing written");
        CHECK(S::writeValue() && F::writes == 0 && !S::pending()
              && S::link() == Kvasir::Link::answering);
    }

    {
        test("a refused write stays pending, logged once, retried on the 1/2/4 s schedule");
        S::value()   = 42;
        F::failsLeft = 3;
        CHECK(!S::writeValue());
        CHECK(S::pending() && S::retry().failures == 1 && S::retry().last == E::refused);
        CHECK(S::failedLogs == 1 && S::lastLogged == E::refused);
        CHECK(S::link() == Kvasir::Link::starting);
        TestClock::advance(ms{999});
        S::turn();
        CHECK(F::writes == 1);   // not due yet
        TestClock::advance(ms{1});
        S::turn();   // due: second failure, next in 2 s
        CHECK(F::writes == 2 && S::retry().failures == 2);
        TestClock::advance(ms{1999});
        S::turn();
        CHECK(F::writes == 2);
        TestClock::advance(ms{1});
        S::turn();   // third failure: absent
        CHECK(F::writes == 3 && S::link() == Kvasir::Link::absent);
        TestClock::advance(ms{4000});
        S::turn();   // lands
        CHECK(F::writes == 4 && F::stored == 42 && !S::pending() && S::recoveredLogs == 1);
        CHECK(S::link() == Kvasir::Link::answering && S::failedLogs == 3);
        TestClock::advance(ms{60000});
        S::turn();
        CHECK(F::writes == 4);   // nothing pending: turn() does nothing
    }

    {
        test("a write that reads back wrong is verify, and pending");
        S::value()   = 43;
        F::dropsLeft = 1;
        CHECK(!S::writeValue() && S::retry().last == E::verify && S::pending());
        TestClock::advance(ms{1000});
        S::turn();
        CHECK(F::stored == 43 && !S::pending());
    }

    return Kvasir::Test::report();
}
