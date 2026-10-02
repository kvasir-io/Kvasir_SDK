// criticalSection<Tag>(f [, pred]) under the host policy (a mutex per tag): exact counts from two
// threads, unrelated tags that do not contend, the predicate form, what the policy concept rejects,
// and results passed through unchanged. Run it under TSan (config tsan) as the other threaded tests.
#include "multicore_test.hpp"

#include <atomic>
#include <cstdint>
#include <kvasir/Atomic/CriticalSection.hpp>
#include <thread>
#include <type_traits>

namespace {
using Kvasir::criticalSection;
using Kvasir::Atomic::CriticalSectionPolicy;

// ---- the concept -------------------------------------------------------------------------------

struct Decays {   // returns a copy: loses int&&

    template<typename,
             typename F>
    static auto run(F&& f) {
        return f();
    }

    template<typename,
             typename F,
             typename P>
    static auto run(F&& f,
                    P&&) {
        return f();
    }
};

struct NoPredicate {
    template<typename,
             typename F>
    static decltype(auto) run(F&& f) {
        return f();
    }
};

struct NoTag {
    template<typename F>
    static decltype(auto) run(F&& f) {
        return f();
    }

    template<typename F,
             typename P>
    static decltype(auto) run(F&& f,
                              P&&) {
        return f();
    }
};

static_assert(!CriticalSectionPolicy<Decays>);
static_assert(!CriticalSectionPolicy<NoPredicate>);
static_assert(!CriticalSectionPolicy<NoTag>);
static_assert(CriticalSectionPolicy<Kvasir::Atomic::Policy::MutexPerTag>);
static_assert(CriticalSectionPolicy<Kvasir::Atomic::Policy::IrqMask>);
static_assert(CriticalSectionPolicy<Kvasir::Atomic::Policy::IrqMaskAndLock>);

// ---- results pass through ----------------------------------------------------------------------

int stored = 7;

void resultsPassThrough() {
    Kvasir::Test::test("results pass through");
    int& ref = criticalSection([]() -> int& { return stored; });
    CHECK(&ref == &stored);
    static_assert(std::is_same_v<decltype(criticalSection([]() -> int& { return stored; })), int&>);
    static_assert(std::is_same_v<decltype(criticalSection([] { return 1; })), int>);
    static_assert(std::is_same_v<decltype(criticalSection([] {})), void>);
    CHECK(criticalSection([] { return 41; }) + 1 == 42);
}

// ---- exclusion -----------------------------------------------------------------------------------

constexpr std::uint32_t Iterations = 100'000;

template<typename Tag>
void exactCount(char const* name) {
    Kvasir::Test::test(name);
    std::uint32_t counter = 0;
    auto          worker  = [&] {
        for(std::uint32_t i = 0; i < Iterations; ++i) {
            criticalSection<Tag>([&] { ++counter; });
        }
    };
    std::thread a{worker};
    std::thread b{worker};
    a.join();
    b.join();
    CHECK(counter == 2 * Iterations);
}

struct CountTag {};

struct TagOne {};

struct TagTwo {};

// Thread A holds tag one and waits for a flag that thread B sets while holding tag two: this
// deadlocks if the two tags shared a lock.
void unrelatedTagsDoNotContend() {
    Kvasir::Test::test("unrelated tags do not contend");
    std::atomic<bool> aInside{false};
    std::atomic<bool> bDone{false};
    std::thread       a{[&] {
        criticalSection<TagOne>([&] {
            aInside = true;
            while(!bDone) { std::this_thread::yield(); }
        });
    }};
    while(!aInside) { std::this_thread::yield(); }
    std::thread b{[&] { criticalSection<TagTwo>([&] { bDone = true; }); }};
    b.join();
    a.join();
    CHECK(bDone);
}

// The predicate form: take a value another thread produces, with no gap between test and take.
void predicateWaits() {
    Kvasir::Test::test("predicate waits");

    struct DataTag {};

    int  value = 0;
    bool ready = false;   // guarded by DataTag, like value

    std::thread producer{[&] {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
        criticalSection<DataTag>([&] {
            value = 42;
            ready = true;
        });
    }};
    int const   got = criticalSection<DataTag>(
      [&] {
          ready = false;
          return value;
      },
      [&] { return ready; });
    producer.join();
    CHECK(got == 42);
    CHECK(!ready);
}
}   // namespace

int main() {
    resultsPassThrough();
    exactCount<Kvasir::Atomic::GlobalSection>("exact count, untagged");
    exactCount<CountTag>("exact count, tagged");
    unrelatedTagsDoNotContend();
    predicateWaits();
    return Kvasir::Test::finish();
}
