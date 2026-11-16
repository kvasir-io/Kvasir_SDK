// Kvasir::PaddingFree: what it takes and what it refuses, decided at compile time.
#include "kvasir/Util/PaddingFree.hpp"
#include "test_harness.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace {
using Kvasir::PaddingFree;

enum class Kind : std::uint8_t { a, b };

struct Packed {
    std::uint32_t a;
    std::uint16_t b;
    std::uint16_t c;
};

struct Gap {   // three bytes between a and b
    std::uint8_t  a;
    std::uint32_t b;
};

struct WithFloat {
    float         gain;
    std::uint32_t count;
};

struct FloatThenTail {   // three bytes behind b
    float        gain;
    std::uint8_t b;
};

struct Nested {
    WithFloat inner;
    double    scale;
};

struct NestedGap {
    std::uint8_t a;
    WithFloat    inner;
};

struct Named {   // the padding named where the compiler put it
    Kind         kind;
    std::uint8_t reserved[3];
    float        value;
};

struct Tables {
    std::array<WithFloat, 2> rows;
    float                    tail[2];
};

struct TablesOfGaps {
    std::array<FloatThenTail, 2> rows;
};

union Either {
    float         f;
    std::uint32_t u;
};

struct Built {   // a declared constructor: no aggregate, the check cannot look into it
    float value{};

    constexpr Built() = default;
};

struct Declared {
    float value{};

    constexpr Declared() = default;
};
}   // namespace

template<>
inline constexpr bool Kvasir::declaredPaddingFree<Declared> = true;

namespace {
// scalars
static_assert(PaddingFree<std::uint8_t> && PaddingFree<std::int64_t> && PaddingFree<bool>
              && PaddingFree<Kind>);
static_assert(PaddingFree<float> && PaddingFree<double>);
#ifdef __x86_64__
static_assert(!PaddingFree<long double>,
              "the x87 format leaves six bytes unused");
#endif
// structs of integers: std::has_unique_object_representations decides
static_assert(PaddingFree<Packed>);
static_assert(!PaddingFree<Gap>);
// structs with floating point: by their members
static_assert(PaddingFree<WithFloat>);
static_assert(!PaddingFree<FloatThenTail>);
static_assert(PaddingFree<Nested>);
static_assert(!PaddingFree<NestedGap>);
static_assert(PaddingFree<Named>);
// arrays, in and out of a struct, cv-qualified
static_assert(PaddingFree<std::array<float,
                                     3>>
              && PaddingFree<float[3]> && PaddingFree<WithFloat const>);
static_assert(PaddingFree<Tables>);
static_assert(!PaddingFree<TablesOfGaps>);
static_assert(!PaddingFree<std::array<Gap,
                                      2>>
              && !PaddingFree<Gap[2]>);
// what the check cannot look into is refused until its author says otherwise
static_assert(!PaddingFree<Either>);
static_assert(!PaddingFree<Built>);
static_assert(PaddingFree<Declared>);

void decidedAtCompileTime() {
    Kvasir::Test::test("decidedAtCompileTime");
    CHECK(PaddingFree<WithFloat>);
    CHECK(!PaddingFree<Gap>);
}
}   // namespace

int main() {
    decidedAtCompileTime();
    return Kvasir::Test::report();
}
