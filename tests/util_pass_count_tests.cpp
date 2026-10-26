// PassCount: 0 only before the first pass, and past 2^32 - 1 on at 1, never back to 0 (ImageCheck's and
// StackHighWater's pass counters, whose readers take 0 for "not counted yet"). Constant evaluation and run time.
#include "kvasir/Util/PassCount.hpp"
#include "test_harness.hpp"

#include <cstdint>
#include <limits>

using Kvasir::PassCount;

namespace {
constexpr std::uint32_t max = std::numeric_limits<std::uint32_t>::max();

static_assert(PassCount{}.none() && PassCount{}.value() == 0);
static_assert((++PassCount{}).value() == 1);
static_assert(!(++PassCount{}).none());
static_assert((++PassCount{41}).value() == 42);
static_assert((++PassCount{max - 1}).value() == max);
static_assert((++PassCount{max}).value() == 1,
              "wraps to 1, not 0");
static_assert(!(++PassCount{max}).none());

void wrapsAtRunTime() {
    std::uint32_t volatile start = max - 2;
    PassCount c{start};
    for(int i = 0; i < 4; ++i) {
        ++c;
        CHECK(!c.none());
    }
    CHECK(c.value() == 2);   // Max - 1, Max, 1, 2
}
}   // namespace

int main() {
    wrapsAtRunTime();
    return Kvasir::Test::report();
}
