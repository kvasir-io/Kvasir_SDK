// Runtime/StackUsage.hpp over a stack of its own (a word buffer, no linker symbols): the paint below the SP leaves the
// StackProtector's sentinel run at the bottom and everything from the end up alone, free() counts the painted run, a
// stack used deeper counts less, a sentinel value further up is painted over; StackHighWater's turns of N words come
// to the same free() as one scan, pass after pass.
#include "kvasir/Runtime/StackUsage.hpp"
#include "test_harness.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace {
namespace D = Kvasir::StackUsageDetail;

constexpr std::size_t   words    = 64;
constexpr std::size_t   painted  = 48;   // the paint ends here (the SP minus the margin)
constexpr std::uint32_t other    = 0x1234'5678U;
constexpr std::uint32_t sentinel = Kvasir::StackUsage::protectorSentinel;

std::array<std::uint32_t, words> stack{};

// StackHighWater's Stack: this buffer
struct FakeStack {
    static std::uint32_t* low() { return stack.data(); }

    static std::uint32_t* high() { return stack.data() + stack.size(); }

    static std::size_t size() { return stack.size() * sizeof(std::uint32_t); }

    static void runtimeInit() { D::paint(low(), low() + painted); }
};

void fill(std::uint32_t v) { stack.fill(v); }

void paintAndCount() {
    using Kvasir::Test::test;
    test("paint: below the end only, past the sentinel run at the bottom");
    fill(other);
    stack[0] = sentinel;
    stack[1] = sentinel;
    stack[5] = sentinel;   // not part of the bottom run: painted over
    FakeStack::runtimeInit();
    CHECK_EQ(stack[0], sentinel);
    CHECK_EQ(stack[1], sentinel);
    for(std::size_t i = 2; i < painted; ++i) { CHECK_EQ(stack[i], Kvasir::StackUsage::pattern); }
    for(std::size_t i = painted; i < words; ++i) { CHECK_EQ(stack[i], other); }

    test("free: the painted run, without the sentinel");
    CHECK_EQ(D::freeBytes(FakeStack::low(), FakeStack::high()), (painted - 2) * 4U);

    test("free: a deeper stack overwrote the top of the run");
    for(std::size_t i = painted - 10; i < painted; ++i) { stack[i] = other; }
    CHECK_EQ(D::freeBytes(FakeStack::low(), FakeStack::high()), (painted - 12) * 4U);

    test("free: no sentinel, and nothing painted at all");
    fill(other);
    FakeStack::runtimeInit();
    CHECK_EQ(D::freeBytes(FakeStack::low(), FakeStack::high()), painted * 4U);
    fill(other);
    CHECK_EQ(D::freeBytes(FakeStack::low(), FakeStack::high()), 0U);
}

void highWater() {
    using Kvasir::Test::test;
    using HighWater = Kvasir::StackHighWater<5, FakeStack>;
    test("StackHighWater: size() until the first pass, then the scan's free()");
    fill(other);
    stack[0] = sentinel;
    HighWater::runtimeInit();
    CHECK_EQ(HighWater::passes(), 0U);
    CHECK_EQ(HighWater::free(), words * 4U);
    // 5 words a turn: the run ends at `painted`, so the pass ends in the turn that reaches it
    std::size_t turns = 0;
    while(HighWater::passes() == 0 && turns < 100) {
        HighWater::turn();
        ++turns;
    }
    CHECK_EQ(HighWater::passes(), 1U);
    CHECK_EQ(turns, painted / 5U + 1U);
    CHECK_EQ(HighWater::free(), D::freeBytes(FakeStack::low(), FakeStack::high()));
    CHECK_EQ(HighWater::free(), (painted - 1) * 4U);

    test("StackHighWater: the next pass sees the deeper stack");
    for(std::size_t i = painted - 7; i < painted; ++i) { stack[i] = other; }
    while(HighWater::passes() == 1) { HighWater::turn(); }
    CHECK_EQ(HighWater::free(), (painted - 8) * 4U);
}
}   // namespace

int main() {
    paintAndCount();
    highWater();
    return Kvasir::Test::report();
}
