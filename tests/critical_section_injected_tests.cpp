// The injection idiom: a policy specialised AFTER the header and after a function that calls
// criticalSection is the one that runs (conc's "injected custom policy is used before definition").
#include "test_harness.hpp"

#include <kvasir/Atomic/CriticalSection.hpp>

namespace {
int runs = 0;

struct CountingPolicy {
    template<typename,
             typename F>
    static decltype(auto) run(F&& f) {
        ++runs;
        return std::forward<F>(f)();
    }

    template<typename,
             typename F,
             typename P>
    static decltype(auto) run(F&& f,
                              P&& p) {
        ++runs;
        while(!p()) {}
        return std::forward<F>(f)();
    }
};

// uses criticalSection before the specialisation below is declared
int useBefore() {
    return Kvasir::criticalSection([] { return 3; });
}
}   // namespace

template<>
inline constexpr auto Kvasir::Atomic::injectedCriticalSection<> = CountingPolicy{};

int main() {
    Kvasir::Test::test("injected policy is used before its definition");
    CHECK(useBefore() == 3);
    CHECK(Kvasir::criticalSection([] { return 4; }, [] { return true; }) == 4);
    CHECK(runs == 2);
    return Kvasir::Test::report();
}
