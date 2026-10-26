// Kvasir::Health::LinkCheck / PolledCheck / Of / checkInIf (Health/LinkCheck.hpp): a check fed from a part's link. A
// list of entries stands in for the Startup list: the supervisor reads its checks through the same Filter<OfHook>
// over the entries' Extends that Startup::ExtendsOn uses, and a turn runs Hook::MainLoop through
// Detail::dispatch, as Startup::run does, then service(). Cases: answering beats, starting and absent do not
// (graceAfterBoot covers a bring-up), offline beats unless the Config opts out, a part back in time is never starved,
// the four links through checkInIf, an object part, a predicate check, and the Config's onStarved ladder with a part
// that comes back during its window.
#include "kvasir/Health/Health.hpp"

#include "fixtures/Health.hpp"
#include "kvasir/Health/LinkCheck.hpp"
#include "test_harness.hpp"

#include <chrono>

namespace H = Kvasir::Health;
using Kvasir::Link;
using namespace std::chrono_literals;

namespace {
using namespace Kvasir::Test::HealthFixture;

// a Startup over the entries Ps...: ExtendsOn and run<Hook> as the real one computes them
template<typename... Ps>
struct ListStartup {
    using Entries = brigand::list<Ps...>;

    template<typename Hook>
    using ExtendsOn
      = Kvasir::MPL::Filter<Kvasir::Startup::Detail::OfHook<Hook>,
                            Kvasir::MPL::Expand<Kvasir::Startup::Detail::AllExtends, Entries>>;

    template<typename Hook>
    static void run() {
        if constexpr(!std::is_same_v<Hook, Kvasir::Hook::AllChecksReported>) {
            Kvasir::Startup::Detail::dispatch<Hook, void>(static_cast<Entries*>(nullptr));
        }
    }
};

template<typename S, typename... Ps>
using SupOf = H::Supervisor<ListStartup<Ps...>, Watchdog<S>, FakeClock, Period10>;

// one loop turn: 10 ms, the MainLoop entries, then the supervisor
template<typename Sup,
         typename... Ps>
void loopTurns(int n) {
    for(int i = 0; i < n; ++i) {
        advance(10ms);
        ListStartup<Ps...>::template run<Kvasir::Hook::MainLoop>();
        Sup::service();
    }
}

// a part whose link the test sets
template<typename Tag>
struct Part {
    static inline Link now = Link::answering;

    static Link link() { return now; }
};

template<typename Tag>
struct C50 {
    static constexpr std::string_view name        = "part";
    static constexpr auto             maxInterval = 50ms;
};

template<typename Tag>
struct NoOffline {
    static constexpr std::string_view name         = "nooff";
    static constexpr auto             maxInterval  = 50ms;
    static constexpr bool             offlineBeats = false;
};

template<typename Tag>
struct Graced {
    static constexpr std::string_view name           = "graced";
    static constexpr auto             maxInterval    = 50ms;
    static constexpr auto             graceAfterBoot = 200ms;
};

void checkInIfValues() {
    using Kvasir::Test::test;
    test(
      "checkInIf: answering and offline beat, starting and absent do not; offlineBeats = false "
      "drops offline");

    struct T {};

    using C               = C50<T>;
    using NO              = NoOffline<T>;
    auto const beatsAfter = [](Link l) {
        auto const b = H::Check<C>::beats;
        H::checkInIf<C>(l);
        return H::Check<C>::beats - b;
    };
    CHECK(beatsAfter(Link::answering) == 1);
    CHECK(beatsAfter(Link::offline) == 1);
    CHECK(beatsAfter(Link::starting) == 0);
    CHECK(beatsAfter(Link::absent) == 0);
    auto const b = H::Check<NO>::beats;
    H::checkInIf<NO>(Link::offline);
    H::checkInIf<NO>(Link::starting);
    CHECK(H::Check<NO>::beats == b);
    H::checkInIf<NO>(Link::answering);
    CHECK(H::Check<NO>::beats == b + 1);
}

void listedEntry() {
    using Kvasir::Test::test;
    test("LinkCheck: its Extends put Check<C> on HealthCheck and its turn() on MainLoop");

    struct T {};

    using E = H::LinkCheck<Part<T>, C50<T>>;
    static_assert(std::is_same_v<ListStartup<E>::ExtendsOn<Kvasir::Hook::HealthCheck>,
                                 brigand::list<H::Check<C50<T>>>>);
    static_assert(brigand::size<ListStartup<E>::ExtendsOn<Kvasir::Hook::MainLoop>>::value == 1);
    auto const b = H::Check<C50<T>>::beats;
    ListStartup<E>::run<Kvasir::Hook::MainLoop>();
    CHECK(H::Check<C50<T>>::beats == b + 1);
}

void answeringNeverStarves() {
    using Kvasir::Test::test;
    test("LinkCheck: a part answering every turn is never starved, and the watchdog is fed");

    struct T {};

    using E   = H::LinkCheck<Part<T>, C50<T>>;
    using Sup = SupOf<T, E>;
    Calls const c{};
    loopTurns<Sup, E>(100);
    CHECK(c.count() == 0);
    CHECK(Watchdog<T>::feeds > 90);
}

void absentStarves() {
    using Kvasir::Test::test;
    test(
      "LinkCheck: a part absent past maxInterval starves the check by name; one back in time does "
      "not");
    {
        struct T {};

        using E   = H::LinkCheck<Part<T>, C50<T>>;
        using Sup = SupOf<T, E>;
        Calls const c{};
        loopTurns<Sup, E>(10);
        Part<T>::now = Link::absent;
        loopTurns<Sup, E>(3);   // 30 ms: inside the window
        Part<T>::now = Link::answering;
        loopTurns<Sup, E>(10);
        CHECK(c.count() == 0);
        Part<T>::now = Link::absent;
        loopTurns<Sup, E>(8);   // 80 ms > 50 ms + one period
        CHECK(c.count() == 1 && c.last().name == "part");
    }
    test("LinkCheck: starting through graceAfterBoot is not starved; still starting after it is");
    {
        struct T {};

        Part<T>::now = Link::starting;
        using E      = H::LinkCheck<Part<T>, Graced<T>>;
        using Sup    = SupOf<T, E>;
        Calls const c{};
        loopTurns<Sup, E>(20);   // 200 ms of the 250 ms first window
        CHECK(c.count() == 0);
        loopTurns<Sup, E>(8);
        CHECK(c.count() == 1 && c.last().name == "graced");
    }
}

void offline() {
    using Kvasir::Test::test;
    test("LinkCheck: offline held for 3 x maxInterval beats; with offlineBeats = false it starves");
    {
        struct T {};

        Part<T>::now = Link::offline;
        using E      = H::LinkCheck<Part<T>, C50<T>>;
        using Sup    = SupOf<T, E>;
        Calls const c{};
        loopTurns<Sup, E>(15);
        CHECK(c.count() == 0);
    }
    {
        struct T {};

        Part<T>::now = Link::offline;
        using E      = H::LinkCheck<Part<T>, NoOffline<T>>;
        using Sup    = SupOf<T, E>;
        Calls const c{};
        loopTurns<Sup, E>(15);
        CHECK(c.count() == 1 && c.last().name == "nooff");
    }
}

// an object part, of static storage duration
struct Sensor {
    Link l = Link::answering;

    [[nodiscard]] Link link() const { return l; }
};

Sensor sensor{};

void objectPart() {
    using Kvasir::Test::test;
    test("LinkCheck<Of<object>>: the object's link() feeds the check");

    struct T {};

    using E   = H::LinkCheck<H::Of<sensor>, C50<T>>;
    using Sup = SupOf<T, E>;
    Calls const c{};
    loopTurns<Sup, E>(10);
    CHECK(c.count() == 0);
    sensor.l = Link::absent;
    loopTurns<Sup, E>(8);
    CHECK(c.count() == 1 && c.last().name == "part");
}

bool allWritten = true;

bool written() { return allWritten; }

void polled() {
    using Kvasir::Test::test;
    test("PolledCheck: a true predicate beats, a false one held past maxInterval starves");

    struct T {};

    using E   = H::PolledCheck<&written, C50<T>>;
    using Sup = SupOf<T, E>;
    Calls const c{};
    loopTurns<Sup, E>(10);
    CHECK(c.count() == 0);
    allWritten = false;
    loopTurns<Sup, E>(8);
    CHECK(c.count() == 1 && c.last().name == "part");
}

// a check with a one-rung ladder whose onStarved brings the part back
template<typename Tag>
struct Restarting {
    static constexpr std::string_view name           = "restart";
    static constexpr auto             maxInterval    = 50ms;
    static constexpr auto             recoveryWindow = 50ms;
    static inline int                 calls          = 0;

    static void onStarved() {
        ++calls;
        Part<Tag>::now = Link::answering;
    }

    // read through a member: clang 23 counts a read of an internal-linkage template's static from outside as no use
    // (-Wunused-but-set-global)
    static int count() { return calls; }
};

void ladder() {
    using Kvasir::Test::test;
    test(
      "LinkCheck + onStarved: the ladder restarts the part, it answers within the window, no "
      "policy call");

    struct T {};

    using C   = Restarting<T>;
    using E   = H::LinkCheck<Part<T>, C>;
    using Sup = SupOf<T, E>;
    Calls const c{};
    loopTurns<Sup, E>(10);
    Part<T>::now = Link::absent;
    loopTurns<Sup, E>(20);
    CHECK(C::count() == 1);
    CHECK(c.count() == 0);
    CHECK(Part<T>::now == Link::answering);
}
}   // namespace

template<>
struct Kvasir::Health::StarvationReaction<Kvasir::Tag::User> {
    using type = Kvasir::Test::HealthFixture::TestPolicy;
};

int main() {
    checkInIfValues();
    listedEntry();
    answeringNeverStarves();
    absentStarves();
    offline();
    objectPart();
    polled();
    ladder();
    return Kvasir::Test::report();
}
