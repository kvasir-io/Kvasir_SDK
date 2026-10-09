// Tests for Kvasir::StaticFunction, a type erased callable with fixed inline storage.
#include "kvasir/Util/StaticFunction.hpp"
#include "test_harness.hpp"

#include <atomic>
#include <chrono>
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

}   // namespace

static void emptyState() {
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

static void capturelessLambda() {
    Kvasir::Test::test("capturelessLambda");

    StaticFunction<int(int), 16> f{[](int v) { return v + 1; }};
    CHECK(static_cast<bool>(f));
    CHECK_EQ(f(1), 2);
    CHECK_EQ(f(41), 42);
}

static void capturingLambda() {
    Kvasir::Test::test("capturingLambda");

    // deliberately not const: a const local is a constant expression and would be
    // folded into the lambda rather than captured, which is not what we want to test
    int                          offset = 10;
    StaticFunction<int(int), 16> f{[offset](int v) { return v + offset; }};
    CHECK_EQ(f(1), 11);

    // several captured values
    int                          a = 1;
    int                          b = 2;
    int                          c = 3;
    StaticFunction<int(int), 32> g{[a, b, c](int v) { return v + a + b + c; }};
    CHECK_EQ(g(10), 16);

    // a capture by reference: the referenced object must outlive the StaticFunction
    int                        counter = 0;
    StaticFunction<void(), 16> h{[&counter] { ++counter; }};
    h();
    h();
    CHECK_EQ(counter, 2);
}

static void functionPointerAndFunctor() {
    Kvasir::Test::test("functionPointerAndFunctor");

    StaticFunction<int(int, int), 16> f{&freeFunction};
    CHECK_EQ(f(20, 22), 42);

    StaticFunction<void(), 16> v{&voidFunction};
    CHECK(static_cast<bool>(v));
    v();

    StaticFunction<int(int), 16> g{Functor{5}};
    CHECK_EQ(g(37), 42);
}

static void returnAndArgumentTypes() {
    Kvasir::Test::test("returnAndArgumentTypes");

    // void return
    int                           sideEffect = 0;
    StaticFunction<void(int), 16> v{[&sideEffect](int x) { sideEffect = x; }};
    v(7);
    CHECK_EQ(sideEffect, 7);

    // no arguments
    StaticFunction<int(), 16> n{[] { return 3; }};
    CHECK_EQ(n(), 3);

    // several arguments of mixed types
    StaticFunction<int(int, char, bool), 16> m{
      [](int i, char c, bool flag) { return flag ? i + c : i - c; }};
    CHECK_EQ(m(10, char{5}, true), 15);
    CHECK_EQ(m(10, char{5}, false), 5);

    // reference arguments
    StaticFunction<void(int&), 16> r{[](int& out) { out = 99; }};
    int                            target = 0;
    r(target);
    CHECK_EQ(target, 99);
}

static void reassignment() {
    Kvasir::Test::test("reassignment");

    StaticFunction<int(int), 24> f{[](int v) { return v + 1; }};
    CHECK_EQ(f(1), 2);

    // assigning a different callable replaces the target
    f = [](int v) { return v * 10; };
    CHECK_EQ(f(3), 30);

    // ... including one with captures
    int offset = 100;
    f          = [offset](int v) { return v + offset; };
    CHECK_EQ(f(1), 101);

    // ... and a plain function pointer
    f = +[](int v) { return v - 1; };
    CHECK_EQ(f(10), 9);
}

static void copyAndMove() {
    Kvasir::Test::test("copyAndMove");

    int                                offset = 7;
    StaticFunction<int(int), 16> const original{[offset](int v) { return v + offset; }};

    // copy construction from a const lvalue
    StaticFunction<int(int), 16> copy{original};
    CHECK(static_cast<bool>(copy));
    CHECK_EQ(copy(1), 8);
    CHECK_EQ(original(1), 8);   // the source still works

    // copy assignment
    StaticFunction<int(int), 16> assigned;
    assigned = original;
    CHECK_EQ(assigned(2), 9);

    // move construction and assignment (the target is trivially copyable, so a move is a
    // copy and the source stays usable)
    StaticFunction<int(int), 16> source{[](int v) { return v * 3; }};
    StaticFunction<int(int), 16> moved{std::move(source)};
    CHECK_EQ(moved(4), 12);

    StaticFunction<int(int), 16> moveAssigned;
    StaticFunction<int(int), 16> source2{[](int v) { return v * 4; }};
    moveAssigned = std::move(source2);
    CHECK_EQ(moveAssigned(4), 16);

    // an empty function copies as empty
    StaticFunction<int(int), 16> const emptyOriginal;
    StaticFunction<int(int), 16>       emptyCopy{emptyOriginal};
    CHECK(!emptyCopy);
}

// copying from a *non const* lvalue must select the copy constructor rather than the
// generic StaticFunction(F&&) one, which would otherwise try to store the StaticFunction
// inside itself
static void copyFromNonConstLvalue() {
    Kvasir::Test::test("copyFromNonConstLvalue");

    StaticFunction<int(int), 16> source{[](int v) { return v + 1; }};

    StaticFunction<int(int), 16> copy{source};
    CHECK(static_cast<bool>(copy));
    CHECK_EQ(copy(1), 2);

    StaticFunction<int(int), 16> assigned;
    assigned = source;
    CHECK_EQ(assigned(10), 11);

    // the source is still callable
    CHECK_EQ(source(100), 101);
}

// a StaticFunction may be widened into one with a bigger storage buffer
static void conversionToLargerStorage() {
    Kvasir::Test::test("conversionToLargerStorage");

    int offset = 5;

    StaticFunction<int(int), 8> const small{[offset](int v) { return v + offset; }};
    CHECK_EQ(small(1), 6);

    // converting construction
    StaticFunction<int(int), 32> wide{small};
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
static void storageSizes() {
    Kvasir::Test::test("storageSizes");

    // a captureless lambda is an empty type, but its size is still 1
    StaticFunction<int(int), 1> tiny{[](int v) { return v; }};
    CHECK_EQ(tiny(5), 5);

    // exactly the size of the captured state
    std::int32_t                                value = 0x1234;
    StaticFunction<int(), sizeof(std::int32_t)> exact{[value] { return int(value); }};
    CHECK_EQ(exact(), 0x1234);

    // a larger capture in a larger buffer
    struct Big {
        std::uint64_t a;
        std::uint64_t b;
        std::uint64_t c;
    };

    Big const                           big{1, 2, 3};
    StaticFunction<std::uint64_t(), 32> fat{[big] { return big.a + big.b + big.c; }};
    CHECK_EQ(fat(), 6U);
}

// The slot types the drivers keep in arrays and request queues must stay trivially copyable:
// Atomic::Queue picks its raw-slot layout on it.
static void staysTriviallyCopyable() {
    Kvasir::Test::test("staysTriviallyCopyable");
    static_assert(std::is_trivially_copyable_v<StaticFunction<void(), 4>>);
    static_assert(std::is_trivially_copyable_v<StaticFunction<int(int, char), 32>>);
    static_assert(std::is_trivially_destructible_v<StaticFunction<void(), 4>>);
    CHECK(true);
}

static void publishAndReset() {
    Kvasir::Test::test("publishAndReset");

    StaticFunction<int(int), 16> f;
    CHECK(!f);
    int offset = 3;
    f.publish([offset](int v) { return v + offset; });
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

static void selfReplacementKeepsCaptures() {
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
    std::uint64_t const a = k, b = k + 1, c = ~k, d = k * 3;
    return [a, b, c, d]() { return b == a + 1 && c == ~a && d == a * 3; };
}

auto second(std::uint64_t k) {
    std::uint64_t const a = k, b = k ^ 0x5555'5555'5555'5555ULL, c = k + 7, d = ~(k + 7);
    return [a, b, c, d]() { return b == (a ^ 0x5555'5555'5555'5555ULL) && c == a + 7 && d == ~c; };
}
}   // namespace interrupt

template<typename Publish>
static void hammer(Publish                   publish,
                   std::chrono::milliseconds duration) {
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

    auto const end = std::chrono::steady_clock::now() + duration;
    for(std::uint64_t k = 1; std::chrono::steady_clock::now() < end; ++k) {
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

static void interruptNeverSeesATornFunction() {
    Kvasir::Test::test("interruptNeverSeesATornFunction");
    using namespace interrupt;

    hammer([](auto f) { slot.publish(f); }, std::chrono::milliseconds{1500});
    std::printf("  publish(): %llu calls, %llu between functions, %llu torn\n",
                static_cast<unsigned long long>(calls.load()),
                static_cast<unsigned long long>(empty.load()),
                static_cast<unsigned long long>(torn.load()));
    CHECK(calls.load() > 1000U);   // the handler really did run in the middle of it
    CHECK_EQ(torn.load(), 0U);

    // operator= of a callable goes through publish() too
    hammer([](auto f) { slot = f; }, std::chrono::milliseconds{500});
    CHECK(calls.load() > 300U);
    CHECK_EQ(torn.load(), 0U);
}

// TSan holds an asynchronous signal back until the thread's next intercepted call, so the
// handler never runs between two plain stores: nothing can tear there, and the check that it
// does is left out. The publish() test still runs; it is the plain build that proves it.
#if defined(__SANITIZE_THREAD__)
    #define STATIC_FUNCTION_TEST_SIGNALS_DEFERRED 1
#elif defined(__has_feature)
    #if __has_feature(thread_sanitizer)
        #define STATIC_FUNCTION_TEST_SIGNALS_DEFERRED 1
    #endif
#endif

// The same stress against a plain memberwise assignment, what operator= did before: it must
// tear, or the test above proves nothing.
static void plainAssignmentDoesTear() {
    Kvasir::Test::test("plainAssignmentDoesTear");
    using namespace interrupt;

    hammer(
      [](auto f) {
          Slot const tmp{f};
          // the old operator=: the captures, then the invoker, each a plain store (the
          // invoker one whole word, as before). Volatile only so the compiler keeps this order
          // and does not merge it into the publish() one by accident.
          static_assert(sizeof(Slot) % sizeof(std::uint64_t) == 0);
          auto* const       dst   = reinterpret_cast<std::uint64_t volatile*>(&slot);
          auto const* const src   = reinterpret_cast<std::uint64_t const*>(&tmp);
          std::size_t const words = sizeof(Slot) / sizeof(std::uint64_t);
          for(std::size_t i = 0; i + 1 != words; ++i) { dst[i] = src[i]; }   // captures
          dst[words - 1] = src[words - 1];                                   // invoker
      },
      std::chrono::milliseconds{1500});
    std::printf("  plain copy: %llu calls, %llu torn\n",
                static_cast<unsigned long long>(calls.load()),
                static_cast<unsigned long long>(torn.load()));
#ifdef STATIC_FUNCTION_TEST_SIGNALS_DEFERRED
    CHECK(true);
#else
    CHECK(torn.load() > 0U);
#endif
}

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
