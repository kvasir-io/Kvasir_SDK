// Tests for Kvasir::StaticFunction, a type erased callable with fixed inline storage.
#include "kvasir/Util/StaticFunction.hpp"
#include "test_harness.hpp"

#include <atomic>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <pthread.h>
#include <thread>
#include <type_traits>

using Kvasir::StaticFunction;

namespace {

int freeFunction(int a,
                 int b) {
    return a + b;
}

void voidFunction() {}

struct Functor {
    int offset;

    int operator()(int v) const { return v + offset; }
};

void emptyState() {
    Kvasir::Test::test("emptyState");

    StaticFunction<int(int), 16> f;
    CHECK(!f);
    CHECK(!static_cast<bool>(f));

    // assigning a callable makes it engaged again
    f = [](int v) { return v * 2; };
    CHECK(static_cast<bool>(f));
    CHECK_EQ(f(21), 42);

    // reset clears the target
    f.reset();
    CHECK(!f);
}

void capturelessLambda() {
    Kvasir::Test::test("capturelessLambda");

    StaticFunction<int(int), 16> const f{[](int v) { return v + 1; }};
    CHECK(static_cast<bool>(f));
    CHECK_EQ(f(1), 2);
    CHECK_EQ(f(41), 42);
}

void capturingLambda() {
    Kvasir::Test::test("capturingLambda");

    // init-captures: a captured const local that is a constant expression would be folded
    // into the lambda rather than captured, which is not what we want to test
    StaticFunction<int(int), 16> const f{[offset = 10](int v) { return v + offset; }};
    CHECK_EQ(f(1), 11);

    // several captured values
    StaticFunction<int(int), 32> const g{[a = 1, b = 2, c = 3](int v) { return v + a + b + c; }};
    CHECK_EQ(g(10), 16);

    // a capture by reference: the referenced object must outlive the StaticFunction
    int                              counter = 0;
    StaticFunction<void(), 16> const h{[&counter] { ++counter; }};
    h();
    h();
    CHECK_EQ(counter, 2);
}

void functionPointerAndFunctor() {
    Kvasir::Test::test("functionPointerAndFunctor");

    StaticFunction<int(int, int), 16> const f{&freeFunction};
    CHECK_EQ(f(20, 22), 42);

    StaticFunction<void(), 16> const v{&voidFunction};
    CHECK(static_cast<bool>(v));
    v();

    StaticFunction<int(int), 16> const g{Functor{5}};
    CHECK_EQ(g(37), 42);
}

void returnAndArgumentTypes() {
    Kvasir::Test::test("returnAndArgumentTypes");

    // void return
    int                                 sideEffect = 0;
    StaticFunction<void(int), 16> const v{[&sideEffect](int x) { sideEffect = x; }};
    v(7);
    CHECK_EQ(sideEffect, 7);

    // no arguments
    StaticFunction<int(), 16> const n{[] { return 3; }};
    CHECK_EQ(n(), 3);

    // several arguments of mixed types
    StaticFunction<int(int, char, bool), 16> const m{
      [](int i, char c, bool flag) { return flag ? i + c : i - c; }};
    CHECK_EQ(m(10, char{5}, true), 15);
    CHECK_EQ(m(10, char{5}, false), 5);

    // reference arguments
    StaticFunction<void(int&), 16> const r{[](int& out) { out = 99; }};
    int                                  target = 0;
    r(target);
    CHECK_EQ(target, 99);
}

void reassignment() {
    Kvasir::Test::test("reassignment");

    StaticFunction<int(int), 24> f{[](int v) { return v + 1; }};
    CHECK_EQ(f(1), 2);

    // assigning a different callable replaces the target
    f = [](int v) { return v * 10; };
    CHECK_EQ(f(3), 30);

    // ... including one with captures
    f = [offset = 100](int v) { return v + offset; };
    CHECK_EQ(f(1), 101);

    // ... and a plain function pointer
    f = +[](int v) { return v - 1; };
    CHECK_EQ(f(10), 9);
}

void copyAndMove() {
    Kvasir::Test::test("copyAndMove");

    StaticFunction<int(int), 16> const original{[offset = 7](int v) { return v + offset; }};

    // copy construction from a const lvalue
    StaticFunction<int(int), 16> const copy{original};
    CHECK(static_cast<bool>(copy));
    CHECK_EQ(copy(1), 8);
    CHECK_EQ(original(1), 8);   // the source still works

    // copy assignment
    StaticFunction<int(int), 16> assigned;
    assigned = original;
    CHECK_EQ(assigned(2), 9);

    // move construction and assignment (the target is trivially copyable, so a move is a
    // copy and the source stays usable)
    StaticFunction<int(int), 16>       source{[](int v) { return v * 3; }};
    StaticFunction<int(int), 16> const moved{std::move(source)};
    CHECK_EQ(moved(4), 12);

    StaticFunction<int(int), 16> moveAssigned;
    StaticFunction<int(int), 16> source2{[](int v) { return v * 4; }};
    moveAssigned = std::move(source2);
    CHECK_EQ(moveAssigned(4), 16);

    // an empty function copies as empty
    StaticFunction<int(int), 16> const emptyOriginal;
    StaticFunction<int(int), 16> const emptyCopy{emptyOriginal};
    CHECK(!emptyCopy);
}

// copying from a *non const* lvalue must select the copy constructor rather than the
// generic StaticFunction(F&&) one, which would otherwise try to store the StaticFunction
// inside itself
void copyFromNonConstLvalue() {
    Kvasir::Test::test("copyFromNonConstLvalue");

    StaticFunction<int(int), 16> const source{[](int v) { return v + 1; }};

    StaticFunction<int(int), 16> const copy{source};
    CHECK(static_cast<bool>(copy));
    CHECK_EQ(copy(1), 2);

    StaticFunction<int(int), 16> assigned;
    assigned = source;
    CHECK_EQ(assigned(10), 11);

    // the source is still callable
    CHECK_EQ(source(100), 101);
}

// a StaticFunction may be widened into one with a bigger storage buffer
void conversionToLargerStorage() {
    Kvasir::Test::test("conversionToLargerStorage");

    StaticFunction<int(int), 8> const small{[offset = 5](int v) { return v + offset; }};
    CHECK_EQ(small(1), 6);

    // converting construction
    StaticFunction<int(int), 32> const wide{small};
    CHECK(static_cast<bool>(wide));
    CHECK_EQ(wide(1), 6);
    CHECK_EQ(wide(37), 42);

    // converting assignment
    StaticFunction<int(int), 32> assigned;
    assigned = small;
    CHECK_EQ(assigned(2), 7);

    // the original is unaffected
    CHECK_EQ(small(3), 8);

    // widening an empty function keeps it empty
    StaticFunction<int(int), 8> const  emptySmall;
    StaticFunction<int(int), 24> const emptyWide{emptySmall};
    CHECK(!emptyWide);
}

// storage size accounting: a callable must fit, and the type must stay usable at the
// smallest size that can hold it
void storageSizes() {
    Kvasir::Test::test("storageSizes");

    // a captureless lambda is an empty type, but its size is still 1
    StaticFunction<int(int), 1> const tiny{[](int v) { return v; }};
    CHECK_EQ(tiny(5), 5);

    // exactly the size of the captured state
    std::int32_t                                      value = 0x1234;
    StaticFunction<int(), sizeof(std::int32_t)> const exact{
      [value] { return static_cast<int>(value); }};
    CHECK_EQ(exact(), 0x1234);

    // a larger capture in a larger buffer
    struct Big {
        std::uint64_t a;
        std::uint64_t b;
        std::uint64_t c;
    };

    Big const                                 big{1, 2, 3};
    StaticFunction<std::uint64_t(), 32> const fat{[big] { return big.a + big.b + big.c; }};
    CHECK_EQ(fat(), 6U);
}

// The slot types the drivers keep in arrays and request queues must stay trivially copyable:
// Atomic::Queue picks its raw-slot layout on it.
void staysTriviallyCopyable() {
    Kvasir::Test::test("staysTriviallyCopyable");
    static_assert(std::is_trivially_copyable_v<StaticFunction<void(), 4>>);
    static_assert(std::is_trivially_copyable_v<StaticFunction<int(int, char), 32>>);
    static_assert(std::is_trivially_destructible_v<StaticFunction<void(), 4>>);
    // operator bool is explicit: tested in a condition, never converted in an overload set
    static_assert(!std::is_convertible_v<StaticFunction<void(), 4>, bool>);
    static_assert(std::is_constructible_v<bool, StaticFunction<void(), 4>>);
    CHECK(true);
}

void publishAndReset() {
    Kvasir::Test::test("publishAndReset");

    StaticFunction<int(int), 16> f;
    CHECK(!f);
    f.publish([offset = 3](int v) { return v + offset; });
    CHECK(static_cast<bool>(f));
    CHECK_EQ(f(1), 4);

    // from another StaticFunction, the same size and a smaller one
    StaticFunction<int(int), 16> const same{[](int v) { return v * 2; }};
    f.publish(same);
    CHECK_EQ(f(5), 10);
    StaticFunction<int(int), 8> const smaller{[](int v) { return v - 1; }};
    f.publish(smaller);
    CHECK_EQ(f(5), 4);

    f.reset();
    CHECK(!f);
}

// A function that replaces itself while it runs keeps its own captures to the end: the call
// runs on a copy of them. (Before: the slot's storage was overwritten under the running
// lambda, which then read the replacement's captures.)
namespace selfReplace {
    StaticFunction<std::uint64_t(), 32> slot;
    std::uint64_t                       laterCalls = 0;
}   // namespace selfReplace

void selfReplacementKeepsCaptures() {
    Kvasir::Test::test("selfReplacementKeepsCaptures");
    using namespace selfReplace;

    std::uint64_t a = 0x1111'1111'1111'1111ULL;
    std::uint64_t b = 0x2222'2222'2222'2222ULL;
    slot.publish([a, b]() -> std::uint64_t {
        std::uint64_t x = 0xAAAA'AAAA'AAAA'AAAAULL;
        std::uint64_t y = 0xBBBB'BBBB'BBBB'BBBBULL;
        slot.publish([x, y]() -> std::uint64_t {
            ++laterCalls;
            return x ^ y;
        });
        asm volatile("" : : : "memory");   // read the captures again, after the replacement
        return a + b;
    });

    CHECK_EQ(slot(), 0x3333'3333'3333'3333ULL);   // its own captures, not x and y
    CHECK_EQ(slot(), 0xAAAA'AAAA'AAAA'AAAAULL ^ 0xBBBB'BBBB'BBBB'BBBBULL);
    CHECK_EQ(laterCalls, 1U);
}

// The interrupt model: one thread publishes into a slot, a signal handler on that same thread
// calls it -- the single-core thread/ISR pair publish() is written for (atomic_signal_fence
// is defined for exactly that). A second thread only raises the signal, as fast as it can.
// The two functions have different invokers and four-word captures; each checks that the
// captures it was called with are its own. A torn assignment -- one invoker with the other's
// captures, or captures half written -- fails that check.
namespace interrupt {
    using Slot = StaticFunction<bool(), 32>;
    Slot slot;

    std::atomic<std::uint64_t> calls{0};
    std::atomic<std::uint64_t> empty{0};
    std::atomic<std::uint64_t> torn{0};
    std::atomic<bool>          stop{false};

    void onSignal(int) {
        if(!slot) {
            empty.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if(!slot()) { torn.fetch_add(1, std::memory_order_relaxed); }
        calls.fetch_add(1, std::memory_order_relaxed);
    }

    // a + 1 == b, c == ~a, d == a * 3 for the first kind; a different pattern for the second
    auto first(std::uint64_t k) {
        std::uint64_t const a = k;
        std::uint64_t const b = k + 1;
        std::uint64_t const c = ~k;
        std::uint64_t const d = k * 3;
        return [a, b, c, d]() { return b == a + 1 && c == ~a && d == a * 3; };
    }

    auto second(std::uint64_t k) {
        std::uint64_t const a = k;
        std::uint64_t const b = k ^ 0x5555'5555'5555'5555ULL;
        std::uint64_t const c = k + 7;
        std::uint64_t const d = ~(k + 7);
        return
          [a, b, c, d]() { return b == (a ^ 0x5555'5555'5555'5555ULL) && c == a + 7 && d == ~c; };
    }
}   // namespace interrupt

// Publishes until the handler has run minCalls times: a count, not a time window, so a slow
// or loaded machine only takes longer (ctest's timeout is the hang guard).
template<typename Publish>
void hammer(Publish       publish,
            std::uint64_t minCalls) {
    using namespace interrupt;
    calls = 0;
    empty = 0;
    torn  = 0;
    stop  = false;

    struct sigaction sa{};
    sa.sa_handler = &onSignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGUSR1, &sa, nullptr);

    pthread_t const target = pthread_self();
    std::thread     raiser{[target] {
        while(!stop.load(std::memory_order_relaxed)) { pthread_kill(target, SIGUSR1); }
    }};

    for(std::uint64_t k = 1; calls.load(std::memory_order_relaxed) < minCalls;) {
        for(int i = 0; i != 256; ++i, ++k) {
            if((k & 1U) != 0) {
                publish(first(k));
            } else {
                publish(second(k));
            }
        }
    }
    stop = true;
    raiser.join();
    signal(SIGUSR1, SIG_DFL);
    slot.reset();
}

void interruptNeverSeesATornFunction() {
    Kvasir::Test::test("interruptNeverSeesATornFunction");
    using namespace interrupt;

    hammer([](auto f) { slot.publish(f); }, 1000);
    std::printf("  publish(): %llu calls, %llu between functions, %llu torn\n",
                static_cast<unsigned long long>(calls.load()),
                static_cast<unsigned long long>(empty.load()),
                static_cast<unsigned long long>(torn.load()));
    CHECK(calls.load() >= 1000U);   // the handler really did run in the middle of it
    CHECK_EQ(torn.load(), 0U);

    // operator= of a callable goes through publish() too
    hammer([](auto f) { slot = f; }, 300);
    CHECK(calls.load() >= 300U);
    CHECK_EQ(torn.load(), 0U);
}

// What operator= did before, a plain memberwise assignment, with the interrupt landing between
// its two halves (raise() runs the handler before it returns): it must tear, or the test above
// proves nothing. The kinds alternate, so every call after the first sees the new captures with
// the other kind's invoker; the first finds the slot still empty.
void plainAssignmentDoesTear() {
    Kvasir::Test::test("plainAssignmentDoesTear");
    using namespace interrupt;
    calls = 0;
    empty = 0;
    torn  = 0;

    struct sigaction sa{};
    sa.sa_handler = &onSignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGUSR1, &sa, nullptr);

    slot.reset();
    constexpr std::uint64_t publishes = 256;
    for(std::uint64_t k = 1; k <= publishes; ++k) {
        Slot const tmp = (k & 1U) != 0 ? Slot{first(k)} : Slot{second(k)};
        // the old operator=: the captures, then the invoker, each a plain store (the invoker one
        // whole word, as before). Volatile only so the compiler keeps this order.
        static_assert(sizeof(Slot) % sizeof(std::uint64_t) == 0);
        auto* const       dst   = reinterpret_cast<std::uint64_t volatile*>(&slot);
        auto const* const src   = reinterpret_cast<std::uint64_t const*>(&tmp);
        std::size_t const words = sizeof(Slot) / sizeof(std::uint64_t);
        for(std::size_t i = 0; i + 1 != words; ++i) { dst[i] = src[i]; }   // captures
        raise(SIGUSR1);                                                    // the interrupt
        dst[words - 1] = src[words - 1];                                   // invoker
    }
    signal(SIGUSR1, SIG_DFL);
    slot.reset();

    std::printf("  plain copy: %llu calls, %llu torn\n",
                static_cast<unsigned long long>(calls.load()),
                static_cast<unsigned long long>(torn.load()));
    CHECK_EQ(empty.load(), 1U);
    CHECK_EQ(torn.load(), publishes - 1);
}

}   // namespace

int main() {
    emptyState();
    capturelessLambda();
    capturingLambda();
    functionPointerAndFunctor();
    returnAndArgumentTypes();
    reassignment();
    copyAndMove();
    copyFromNonConstLvalue();
    conversionToLargerStorage();
    storageSizes();
    staysTriviallyCopyable();
    publishAndReset();
    selfReplacementKeepsCaptures();
    interruptNeverSeesATornFunction();
    plainAssignmentDoesTear();

    return Kvasir::Test::report();
}
