// Tests for Kvasir::SaturatingValue: clamping on construction, saturating increment and
// decrement and the overflow handling in operator+= / operator-=.
#include "kvasir/Util/SaturatingValue.hpp"
#include "test_harness.hpp"

#include <cstdint>
#include <limits>

using Kvasir::SaturatingValue;

using U8     = SaturatingValue<std::uint8_t, 10, 200>;
using I8     = SaturatingValue<std::int8_t, -50, 50>;
using FullU8 = SaturatingValue<std::uint8_t,
                               std::numeric_limits<std::uint8_t>::min(),
                               std::numeric_limits<std::uint8_t>::max()>;
using FullI8 = SaturatingValue<std::int8_t,
                               std::numeric_limits<std::int8_t>::min(),
                               std::numeric_limits<std::int8_t>::max()>;

// a default constructed value is ValueType{} run through the clamp, so it is the minimum
// whenever 0 is below the allowed range
static_assert(U8{}.value() == 10);
static_assert(I8{}.value() == 0);
static_assert(SaturatingValue<std::int8_t,
                              5,
                              50>{}
                .value()
              == 5);
static_assert(SaturatingValue<std::int8_t,
                              -50,
                              -5>{}
                .value()
              == -5);

// construction clamps to both ends
static_assert(U8{0}.value() == 10);
static_assert(U8{9}.value() == 10);
static_assert(U8{10}.value() == 10);
static_assert(U8{123}.value() == 123);
static_assert(U8{200}.value() == 200);
static_assert(U8{201}.value() == 200);
static_assert(U8{255}.value() == 200);
static_assert(I8{-128}.value() == -50);
static_assert(I8{127}.value() == 50);

// implicit conversion yields the stored value
static_assert(static_cast<std::uint8_t>(U8{123}) == 123);
static_assert([] {
    std::uint8_t const v = U8{300 % 256};
    return v;
}() == 44);

// comparison is defaulted and therefore compares the stored value
static_assert(U8{20} == U8{20});
static_assert(U8{20} < U8{21});
static_assert(U8{0} == U8{10});      // both clamped to the minimum
static_assert(U8{255} == U8{200});   // both clamped to the maximum

static void increments() {
    Kvasir::Test::test("increments");

    U8 v{198};
    CHECK_EQ((++v).value(), 199);
    CHECK_EQ((++v).value(), 200);
    CHECK_EQ((++v).value(), 200);   // saturates, must not wrap
    CHECK_EQ((++v).value(), 200);

    // saturating at the maximum of the underlying type must not wrap to 0
    FullU8 f{254};
    CHECK_EQ((++f).value(), 255);
    CHECK_EQ((++f).value(), 255);
}

static void decrements() {
    Kvasir::Test::test("decrements");

    U8 v{12};
    CHECK_EQ((--v).value(), 11);
    CHECK_EQ((--v).value(), 10);
    CHECK_EQ((--v).value(), 10);   // saturates, must not wrap to 255
    CHECK_EQ((--v).value(), 10);

    FullU8 f{1};
    CHECK_EQ((--f).value(), 0);
    CHECK_EQ((--f).value(), 0);

    I8 s{-49};
    CHECK((--s).value() == -50);
    CHECK((--s).value() == -50);
}

static void addAssign() {
    Kvasir::Test::test("addAssign");

    U8 v{100};
    v += 50;
    CHECK_EQ(v.value(), 150);

    // clamps at the configured maximum without any overflow of the underlying type
    v += 40;
    CHECK_EQ(v.value(), 190);
    v += 40;
    CHECK_EQ(v.value(), 200);

    // adding to an already saturated value stays at the maximum
    v += 1;
    CHECK_EQ(v.value(), 200);

    // a negative addend goes the other way and clamps at the minimum
    U8 w{100};
    w += -50;
    CHECK_EQ(w.value(), 50);
    w += -100;
    CHECK_EQ(w.value(), 10);
}

static void addAssignOverflow() {
    Kvasir::Test::test("addAssignOverflow");

    // the addition itself overflows the underlying type: the builtin overflow path must
    // saturate at the maximum rather than wrap
    FullU8 v{200};
    v += 200;
    CHECK_EQ(v.value(), 255);

    FullI8 s{100};
    s += 100;
    CHECK(s.value() == 127);

    // a negative addend that underflows the underlying type saturates at the minimum
    FullI8 t{-100};
    t += -100;
    CHECK(t.value() == -128);

    // unsigned underflow through a negative addend
    FullU8 u{10};
    u += -20;
    CHECK_EQ(u.value(), 0);
}

static void subAssign() {
    Kvasir::Test::test("subAssign");

    U8 v{100};
    v -= 50;
    CHECK_EQ(v.value(), 50);
    v -= 30;
    CHECK_EQ(v.value(), 20);
    v -= 30;
    CHECK_EQ(v.value(), 10);
    v -= 1;
    CHECK_EQ(v.value(), 10);

    // a negative subtrahend adds
    U8 w{100};
    w -= -50;
    CHECK_EQ(w.value(), 150);
    w -= -100;
    CHECK_EQ(w.value(), 200);
}

static void subAssignOverflow() {
    Kvasir::Test::test("subAssignOverflow");

    FullU8 v{10};
    v -= 20;
    CHECK_EQ(v.value(), 0);

    FullI8 s{-100};
    s -= 100;
    CHECK(s.value() == -128);

    FullI8 t{100};
    t -= -100;
    CHECK(t.value() == 127);
}

// the arithmetic operators are constexpr, so the same paths must work at compile time
static_assert([] {
    U8 v{100};
    v += 1000;
    return v.value();
}() == 200);

static_assert([] {
    U8 v{100};
    v -= 1000;
    return v.value();
}() == 10);

static_assert([] {
    FullU8 v{250};
    v += 250;
    return v.value();
}() == 255);

int main() {
    increments();
    decrements();
    addAssign();
    addAssignOverflow();
    subAssign();
    subAssignOverflow();

    return Kvasir::Test::report();
}
