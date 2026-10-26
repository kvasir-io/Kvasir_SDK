// Tests for the Kvasir template metaprogramming layer: Mpl/Types.hpp (AlwaysFalse, the IntegralConstant concept) and
// Mpl/Algorithm.hpp (Find). The list helpers are tested in mpl_list_tests.cpp.
//
// All of this is compile time only, so the test is a wall of static_asserts and main() just
// exists so ctest has something to run: if this file compiles, the tests passed.
#include "kvasir/Common/Interrupt.hpp"
#include "kvasir/Io/Types.hpp"
#include "kvasir/Mpl/Algorithm.hpp"
#include "kvasir/Mpl/Types.hpp"
#include "kvasir/Register/Types.hpp"
#include "test_harness.hpp"

#include <cstdint>
#include <type_traits>

namespace mpl = Kvasir::MPL;

namespace {

template<typename A, typename B>
constexpr bool same = std::is_same_v<A, B>;

template<typename... Ts>
using list = brigand::list<Ts...>;

// tag types used as list elements
struct A {};

struct B {};

struct C {};

struct D {};

// a value member but no value_type
struct ValueOnly {
    static constexpr int value = 3;
};

// a value_type and a value, but no std::integral_constant base
struct LooksLikeOne {
    using value_type                  = int;
    static constexpr value_type value = 3;
};

using AField = Kvasir::Register::FieldLocation<Kvasir::Register::Address<0x10, 0, 0, std::uint32_t>,
                                               0x3U,
                                               Kvasir::Register::ReadWriteAccess,
                                               std::uint32_t>;

}   // namespace

// ===========================================================================
// Types.hpp
// ===========================================================================

static_assert(!mpl::AlwaysFalse<A>::value);
static_assert(!mpl::AlwaysFalse<>::value);

// IntegralConstant: std::integral_constant and what derives from it
static_assert(mpl::IntegralConstant<std::integral_constant<int,
                                                           0>>);
static_assert(mpl::IntegralConstant<std::integral_constant<unsigned,
                                                           3>>);
static_assert(mpl::IntegralConstant<std::bool_constant<true>>);
static_assert(mpl::IntegralConstant<std::true_type>);
static_assert(mpl::IntegralConstant<brigand::uint32_t<7>>);
static_assert(mpl::IntegralConstant<brigand::size<list<A,
                                                       B>>>);
// and nothing else
static_assert(!mpl::IntegralConstant<int>);
static_assert(!mpl::IntegralConstant<A>);
static_assert(!mpl::IntegralConstant<list<>>);
static_assert(ValueOnly::value == LooksLikeOne::value);   // both have a value ...
static_assert(!mpl::IntegralConstant<ValueOnly>);
static_assert(!mpl::IntegralConstant<LooksLikeOne>);
static_assert(!mpl::IntegralConstant<Kvasir::Register::FieldValue<AField,
                                                                  1U>>);
static_assert(!mpl::IntegralConstant<Kvasir::Io::Action::PinFunction<1>>);
// the tags derive from std::integral_constant<int, I>: integral constants, but no runtime int (the conversion is
// deleted), and a port's or pin's ::type is the tag itself
static_assert(mpl::IntegralConstant<Kvasir::Nvic::Index<3>>);
static_assert(mpl::IntegralConstant<Kvasir::Io::HwPort<0>>);
static_assert(mpl::IntegralConstant<Kvasir::Io::Pin<3>>);
static_assert(Kvasir::Nvic::Index<3>::value == 3 && Kvasir::Io::Pin<3>{}.value == 3);
static_assert(!std::is_convertible_v<Kvasir::Nvic::Index<3>,
                                     int>);
static_assert(!std::is_convertible_v<Kvasir::Io::HwPort<0>,
                                     int>);
static_assert(!std::is_convertible_v<Kvasir::Io::Pin<3>,
                                     int>);
static_assert(same<Kvasir::Io::HwPort<0>::type,
                   Kvasir::Io::HwPort<0>>);
static_assert(same<Kvasir::Io::Pin<3>::type,
                   Kvasir::Io::Pin<3>>);

// ===========================================================================
// Algorithm.hpp
// ===========================================================================

// --- Find -----------------------------------------------------------------
static_assert(mpl::FindT<list<A,
                              B,
                              C>,
                         A>::value
              == 0);
static_assert(mpl::FindT<list<A,
                              B,
                              C>,
                         B>::value
              == 1);
static_assert(mpl::FindT<list<A,
                              B,
                              C>,
                         C>::value
              == 2);
// a type that is not in the list yields -1
static_assert(mpl::FindT<list<A,
                              B,
                              C>,
                         D>::value
              == -1);
static_assert(mpl::FindT<list<>,
                         A>::value
              == -1);
// the first occurrence wins
static_assert(mpl::FindT<list<A,
                              B,
                              A>,
                         A>::value
              == 0);

int main() {
    // everything above is checked at compile time; reaching main means it all held
    Kvasir::Test::test("mpl");
    return Kvasir::Test::report();
}
