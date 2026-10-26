// Kvasir::Startup::SafeStateOf<S> (StartUp/SafeState.hpp): every Hook::SafeState entry of both cores' lists runs once,
// in list order, and nothing of another hook; an empty list is an empty call. And Detail::safeStateWired, Start<S>'s
// check. The Startup is a stand-in with ExtendsOn, as Startup's lists are not instantiated on the host.
#include "kvasir/StartUp/SafeState.hpp"
#include "test_harness.hpp"

#include <type_traits>

namespace {
using Kvasir::Hook::MainLoop;
using Kvasir::Hook::SafeState;
using Kvasir::Startup::Extend;

int order[8];
int calls = 0;
int loops = 0;

template<int N>
void entry() {
    order[calls++] = N;
}

void loop() { ++loops; }

// core 0's list, then core 1's (ExtendsOn's order), with a MainLoop entry between them
using Core0 = brigand::
  list<Extend<SafeState, &entry<1>>, Extend<MainLoop, &loop>, Extend<SafeState, &entry<2>>>;
using Core1 = brigand::list<Extend<SafeState, &entry<3>>>;

template<typename Hook, typename List>
using OfHook = Kvasir::MPL::Filter<Kvasir::Startup::Detail::OfHook<Hook>, List>;

struct TwoCores {
    template<typename Hook>
    using ExtendsOn = OfHook<Hook, brigand::append<Core0, Core1>>;
};

struct NoSafeState {
    template<typename Hook>
    using ExtendsOn = OfHook<Hook, brigand::list<Extend<MainLoop, &loop>>>;
};

struct OwnStart {
    static void operator()() {}
};

static_assert(Kvasir::Startup::Detail::safeStateWired<NoSafeState,
                                                      Kvasir::Panic::NoFatalStart>(),
              "a list without SafeState entries needs no fatal start");
static_assert(Kvasir::Startup::Detail::safeStateWired<TwoCores,
                                                      Kvasir::Startup::SafeStateOf<TwoCores>>());
static_assert(Kvasir::Startup::Detail::safeStateWired<TwoCores,
                                                      OwnStart>(),
              "an application's own start (one that calls SafeStateOf itself) passes");
static_assert(std::is_same_v<TwoCores::ExtendsOn<SafeState>,
                             brigand::list<Extend<SafeState,
                                                  &entry<1>>,
                                           Extend<SafeState,
                                                  &entry<2>>,
                                           Extend<SafeState,
                                                  &entry<3>>>>);
}   // namespace

int main() {
    Kvasir::Test::test("every SafeState entry of both cores runs once, in list order");
    Kvasir::Startup::SafeStateOf<TwoCores>{}();
    CHECK(calls == 3 && order[0] == 1 && order[1] == 2 && order[2] == 3);

    Kvasir::Test::test("entries on another hook do not run");
    CHECK(loops == 0);

    Kvasir::Test::test("it runs again, the same way (a fatal path runs it at least twice)");
    Kvasir::Startup::SafeStateOf<TwoCores>{}();
    CHECK(calls == 6 && order[3] == 1 && order[5] == 3);

    Kvasir::Test::test("a list without SafeState entries: an empty call");
    Kvasir::Startup::SafeStateOf<NoSafeState>{}();
    CHECK(calls == 6 && loops == 0);
    return Kvasir::Test::report();
}
