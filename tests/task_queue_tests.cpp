#include "multicore_test.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <kvasir/Multicore/TaskQueue.hpp>
#include <thread>

namespace {
struct Config {
    static constexpr std::size_t slots       = 4;
    static constexpr std::size_t captureSize = 32;
    static constexpr std::size_t resultSize  = 16;
    using OverflowPolicy                     = Kvasir::Atomic::OverFlowPolicyIgnore;
};

using Tasks = Kvasir::Multicore::TaskQueue<Config>;

// A worker thread that polls until told to stop, standing in for core 1.
struct Worker {
    std::atomic<bool> stop{false};
    std::thread       thread{[this] {
        while(!stop.load(std::memory_order_acquire)) {
            if(!Tasks::poll()) { std::this_thread::yield(); }
        }
        while(Tasks::poll()) {}
    }};

    ~Worker() {
        stop.store(true, std::memory_order_release);
        thread.join();
    }
};

void resultsComeBack() {
    Kvasir::Test::test("posted tasks return their results");
    Worker        worker{};
    std::uint32_t errors = 0;
    for(std::uint32_t i = 0; i < 20'000; ++i) {
        Tasks::Future<std::uint64_t> f{};
        while(!f.valid()) {
            f = Tasks::post([i] { return std::uint64_t{i} * 3 + 1; });
        }
        if(f.get() != std::uint64_t{i} * 3 + 1) { ++errors; }
    }
    CHECK(errors == 0);
    CHECK(Tasks::freeSlots() == Config::slots);
}

void manyInFlight() {
    Kvasir::Test::test("several futures in flight, collected out of order");
    Worker worker{};
    auto   a = Tasks::post([] { return 1; });
    auto   b = Tasks::post([] { return 2; });
    auto   c = Tasks::post([] { return 3; });
    CHECK(a.valid() && b.valid() && c.valid());
    CHECK(c.get() == 3);
    CHECK(a.get() == 1);
    CHECK(b.get() == 2);
    CHECK(!a.valid());
}

void voidAndAbandoned() {
    Kvasir::Test::test("a dropped future still runs its task and frees its slot");
    Worker           worker{};
    std::atomic<int> ran{0};
    auto*            counter = &ran;
    for(int i = 0; i < 1000; ++i) {
        Tasks::Future<void> f{};
        while(!f.valid()) {
            f = Tasks::post([counter] { counter->fetch_add(1); });
        }
        // dropped here: fire and forget
    }
    // wait for the worker to drain
    for(int i = 0; i < 10'000 && ran.load() != 1000; ++i) { std::this_thread::yield(); }
    CHECK(ran.load() == 1000);
    for(int i = 0; i < 10'000 && Tasks::freeSlots() != Config::slots; ++i) {
        std::this_thread::yield();
    }
    CHECK(Tasks::freeSlots() == Config::slots);

    auto f = Tasks::post([counter] { counter->fetch_add(1); });
    f.get();   // void get() waits and frees
    CHECK(!f.valid());
    CHECK(ran.load() == 1001);
}

void fullQueue() {
    Kvasir::Test::test("post() on a full queue returns an invalid future");
    // no worker: nothing drains
    Tasks::Future<int> keep[Config::slots];
    for(auto& f : keep) {
        f = Tasks::post([] { return 0; });
        CHECK(f.valid());
    }
    auto extra = Tasks::post([] { return 0; });
    CHECK(!extra.valid());
    CHECK(Tasks::freeSlots() == 0);
    {
        Worker worker{};
        for(auto& f : keep) { CHECK(f.get() == 0); }
    }
    CHECK(Tasks::freeSlots() == Config::slots);
}

void mutableAndAligned() {
    Kvasir::Test::test("mutable lambdas and 8-byte-aligned captures");
    Worker worker{};
    double x = 2.5;
    auto   f = Tasks::post([x, n = 0]() mutable {
        ++n;
        return x * 2 + n;
    });
    CHECK(f.get() == 6.0);
}

// A clock one tick further on every now(): the deadline is a number of looks at the clock, not
// a time, so the outcome does not depend on how fast or loaded the machine is.
struct TickClock {
    using duration   = std::chrono::microseconds;
    using rep        = duration::rep;
    using period     = duration::period;
    using time_point = std::chrono::time_point<TickClock, duration>;

    static inline std::atomic<rep> ticks{0};

    static time_point now() { return time_point{duration{ticks.fetch_add(1) + 1}}; }
};

void boundedWait() {
    Kvasir::Test::test("waitUntil times out without a worker and succeeds with one");
    auto f = Tasks::post([] { return 42; });
    // no worker: nothing can make it ready, so it gives up after 20 looks at the clock
    auto const start = TickClock::now();
    CHECK(!f.waitUntil(start + std::chrono::microseconds{20}));
    CHECK(TickClock::now() - start > std::chrono::microseconds{20});
    {
        Worker worker{};
        // a deadline that never comes: true as soon as the worker has run it
        CHECK(f.waitUntil(TickClock::time_point::max()));
    }
    CHECK(f.get() == 42);
}
}   // namespace

int main() {
    resultsComeBack();
    manyInFlight();
    voidAndAbandoned();
    fullQueue();
    mutableAndAligned();
    boundedWait();
    return Kvasir::Test::finish();
}
