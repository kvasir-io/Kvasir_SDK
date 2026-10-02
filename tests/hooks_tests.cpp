// Tests for kvasir/StartUp/Hooks.hpp: functions peripherals add to named hooks, run in list order
// by one call. Startup itself does not instantiate on the host (KVASIR_START is a no-op there), so
// this drives Detail::runHookOf with an explicit list, which is all Startup::run does.
#include "kvasir/StartUp/Hooks.hpp"

#include "kvasir/StartUp/ListRules.hpp"
#include "test_harness.hpp"

#include <vector>

namespace {
using Kvasir::Startup::Extend;
template<typename... Ts>
using list = brigand::list<Ts...>;

std::vector<int> calls;

struct Tick {
    using Signature = void();
};

struct Sample {
    using Signature = void(int);
};

struct Unused {
    using Signature = void();
};

struct First {
    static void tick() { calls.push_back(1); }

    using Extends = Extend<Tick, &First::tick>;
};

struct Second {   // two hooks

    static void tick() { calls.push_back(2); }

    static void sample(int v) { calls.push_back(200 + v); }

    using Extends = list<Extend<Tick, &Second::tick>, Extend<Sample, &Second::sample>>;
};

struct Plain {   // no Extends: a peripheral with a runtime init only

    static void runtimeInit() {}
};

struct Third {
    static void sample(int v) { calls.push_back(300 + v); }

    static void tick() { calls.push_back(3); }

    using Extends = list<Extend<Sample, &Third::sample>, Extend<Tick, &Third::tick>>;
};

struct HookOnly {
    static void tick() {}

    using Extends = Extend<Kvasir::Hook::MainLoop, &HookOnly::tick>;
};

using Periphs = list<First, Plain, Second, Third>;

template<typename Hook,
         typename... Args>
void run(Args const&... args) {
    Kvasir::Startup::Detail::runHookOf<Hook>(static_cast<Periphs*>(nullptr), args...);
}

// a hook-only type is a legal Startup entry
static_assert(Kvasir::Startup::ListRules::isPeripheral<HookOnly>);
// the signature check, both ways
static_assert(Kvasir::Startup::Detail::callableAs<void(int),
                                                  decltype(&Second::sample)>);
static_assert(!Kvasir::Startup::Detail::callableAs<void(),
                                                   decltype(&Second::sample)>);
static_assert(!Kvasir::Startup::Detail::callableAs<void(int),
                                                   decltype(&First::tick)>);
static_assert(std::is_same_v<Kvasir::Startup::Detail::AllExtends<First,
                                                                 Plain>,
                             list<Extend<Tick,
                                         &First::tick>>>);

void listOrder() {
    Kvasir::Test::test("list order, only the hook asked for");
    calls.clear();
    run<Tick>();
    CHECK((calls == std::vector<int>{1, 2, 3}));
}

void withArgument() {
    Kvasir::Test::test("a hook with an argument");
    calls.clear();
    run<Sample>(7);
    CHECK((calls == std::vector<int>{207, 307}));
}

void nobodyExtends() {
    Kvasir::Test::test("a hook nobody extends");
    calls.clear();
    run<Unused>();
    CHECK(calls.empty());
}
}   // namespace

int main() {
    listOrder();
    withArgument();
    nobodyExtends();
    return Kvasir::Test::report();
}
