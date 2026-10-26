// Nvic::IsrsFor (kvasir/Common/Interrupt.hpp): one Isr entry per index of a list, all on one handler - the same
// type a driver's hand-written `brigand::list<Isr<&onIsr, Index<N>>...>` had, so a Startup list sees no change.
#include "kvasir/Common/Interrupt.hpp"
#include "test_harness.hpp"

#include <memory>
#include <type_traits>

namespace {
void onIsr() {}

void other() {}

using Kvasir::Nvic::Index;
using Kvasir::Nvic::Isr;
using Kvasir::Nvic::IsrsFor;
constexpr auto h = std::addressof(onIsr);

static_assert(std::is_same_v<IsrsFor<h,
                                     brigand::list<>>,
                             brigand::list<>>);
static_assert(std::is_same_v<IsrsFor<h,
                                     brigand::list<Index<7>>>,
                             brigand::list<Isr<h,
                                               Index<7>>>>);
static_assert(std::is_same_v<IsrsFor<h,
                                     brigand::list<Index<-1>,
                                                   Index<0>,
                                                   Index<31>>>,
                             brigand::list<Isr<h,
                                               Index<-1>>,
                                           Isr<h,
                                               Index<0>>,
                                           Isr<h,
                                               Index<31>>>>);
// any type with a ::value works as an index, as the drivers' hand-written lists took it
static_assert(std::is_same_v<IsrsFor<h,
                                     brigand::list<std::integral_constant<int,
                                                                          4>>>,
                             brigand::list<Isr<h,
                                               Index<4>>>>);
// a list taken with decltype of a static constexpr member is const, and so are its elements
// (`decltype(Traits::Interrupts)` = `const brigand::list<const Index<4>>`)
static_assert(std::is_same_v<IsrsFor<h,
                                     brigand::list<Index<4>> const>,
                             brigand::list<Isr<h,
                                               Index<4>>>>);
static_assert(std::is_same_v<IsrsFor<h,
                                     brigand::list<Index<4> const> const>,
                             brigand::list<Isr<h,
                                               Index<4>>>>);
// the handler is part of the type
static_assert(!std::is_same_v<IsrsFor<h,
                                      brigand::list<Index<7>>>,
                              IsrsFor<std::addressof(other),
                                      brigand::list<Index<7>>>>);
}   // namespace

int main() {
    Kvasir::Test::test("isrsFor");
    using Two = IsrsFor<h, brigand::list<Index<1>, Index<2>>>;
    CHECK_EQ(brigand::size<Two>::value, 2U);
    return Kvasir::Test::report();
}
