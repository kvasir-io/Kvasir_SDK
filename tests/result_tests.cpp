// kvasir/Util/Result.hpp: Kvasir::Result is std::expected, Kvasir::discard() drops a [[nodiscard]] one without a
// warning (this TU is built with -Wall -Wextra -Wpedantic -Werror on both compilers).
#include "kvasir/Util/Result.hpp"

#include "test_harness.hpp"

#include <expected>
#include <type_traits>

namespace {
enum class E { flash, full };

static_assert(std::is_same_v<Kvasir::Result<int,
                                            E>,
                             std::expected<int,
                                           E>>);
static_assert(std::is_same_v<Kvasir::Result<void,
                                            E>,
                             std::expected<void,
                                           E>>);

[[nodiscard]] Kvasir::Result<int,
                             E>
half(int v) {
    if(v % 2 != 0) { return std::unexpected{E::full}; }
    return v / 2;
}

[[nodiscard]] Kvasir::Result<void,
                             E>
store(bool ok) {
    if(!ok) { return std::unexpected{E::flash}; }
    return {};
}

constexpr Kvasir::Result<int, E> constant = 7;
static_assert(constant.has_value() && *constant == 7,
              "usable in constant expressions");
}   // namespace

int main() {
    Kvasir::Test::test("Result and discard");
    CHECK(half(4).has_value() && *half(4) == 2);
    CHECK(!half(3).has_value() && half(3).error() == E::full);
    CHECK(store(true).has_value());
    CHECK(!store(false).has_value() && store(false).error() == E::flash);
    Kvasir::discard(store(false));   // no warning: the outcome is dropped on purpose
    Kvasir::discard(half(1));
    return Kvasir::Test::report();
}
