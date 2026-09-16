// Tests for the Kvasir::Devices layer: the SM::match helper.
#include "kvasir/Devices/utils.hpp"
#include "kvasir_test.hpp"

#include <variant>

using namespace Kvasir::Test;

// ===========================================================================
// Devices/utils.hpp: overloaded / match
// ===========================================================================

static void matchVisitsTheActiveAlternative() {
    test("matchVisitsTheActiveAlternative");

    using V = std::variant<int, double, char const*>;

    auto describe = [](V const& v) {
        return Kvasir::SM::match(
          v,
          [](int) { return 1; },
          [](double) { return 2; },
          [](char const*) { return 3; });
    };

    CHECK_EQ(describe(V{42}), 1);
    CHECK_EQ(describe(V{4.2}), 2);
    CHECK_EQ(describe(V{"hello"}), 3);
}

static void matchCanMutateAndReturnVoid() {
    test("matchCanMutateAndReturnVoid");

    std::variant<int, double> v{1};
    int                       seen = 0;

    Kvasir::SM::match(
      v,
      [&seen](int& x) {
          seen = x;
          x    = 99;
      },
      [](double&) {});

    CHECK_EQ(seen, 1);
    CHECK_EQ(std::get<int>(v), 99);

    // a generic matcher covers every alternative at once
    int visited = 0;
    Kvasir::SM::match(v, [&visited](auto const&) { ++visited; });
    CHECK_EQ(visited, 1);
}

int main() {
    matchVisitsTheActiveAlternative();
    matchCanMutateAndReturnVoid();

    return Kvasir::Test::report();
}
