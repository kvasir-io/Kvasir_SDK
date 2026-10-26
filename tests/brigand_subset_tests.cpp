// Tests for kvasir/Mpl/brigand.hpp, the subset of brigand Kvasir keeps: every facility's result type, at the
// sizes where the old implementation switched paths (append 16 lists at a time), and the edge cases callers
// rely on (append<T> is T, an empty nested list flattens to nothing, other templates than brigand::list).
//
// Compile time only: if this file compiles, the tests passed; main() exists for ctest.
#include "kvasir/Mpl/brigand.hpp"

#include "test_harness.hpp"

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace {

template<typename A, typename B>
constexpr bool same = std::is_same_v<A, B>;

template<typename... Ts>
using list = brigand::list<Ts...>;

template<int I>
struct E {};

template<typename... Ts>
struct Other {};   // a type list of another template

// list
static_assert(same<list<E<0>,
                        E<1>>::type,
                   list<E<0>,
                        E<1>>>);

// uint32_t, size
static_assert(same<brigand::uint32_t<7>,
                   std::integral_constant<std::uint32_t,
                                          7>>);
static_assert(same<brigand::size<list<>>,
                   std::integral_constant<std::size_t,
                                          0>>);
static_assert(brigand::size<list<E<0>,
                                 E<1>,
                                 E<2>>>::value
              == 3);
static_assert(brigand::size<Other<E<0>>>::value == 1);

// a list of N elements E<0>..E<N-1>
template<typename Seq>
struct MakeList;

template<std::size_t... I>
struct MakeList<std::index_sequence<I...>> {
    using type = list<E<static_cast<int>(I)>...>;
};

template<std::size_t N>
using Iota = typename MakeList<std::make_index_sequence<N>>::type;

// at_c, front over 1..48 elements
template<std::size_t N,
         std::size_t... I>
constexpr bool atAll(std::index_sequence<I...>) {
    return (same<brigand::at_c<Iota<N>, I>, E<static_cast<int>(I)>> && ...);
}

template<std::size_t... N>
constexpr bool atSizes(std::index_sequence<N...>) {
    return (atAll<N + 1>(std::make_index_sequence<N + 1>{}) && ...);
}

static_assert(atSizes(std::make_index_sequence<48>{}));
static_assert(same<brigand::at_c<Other<E<0>,
                                       E<1>>,
                                 1>,
                   E<1>>);
static_assert(same<brigand::front<Iota<9>>,
                   E<0>>);
static_assert(same<brigand::front<Other<E<3>>>,
                   E<3>>);

// push_front, push_back, wrap
static_assert(same<brigand::push_front<list<E<1>>,
                                       E<0>>,
                   list<E<0>,
                        E<1>>>);
static_assert(same<brigand::push_front<list<E<2>>,
                                       E<0>,
                                       E<1>>,
                   list<E<0>,
                        E<1>,
                        E<2>>>);
static_assert(same<brigand::push_back<list<E<0>>,
                                      E<1>,
                                      E<2>>,
                   list<E<0>,
                        E<1>,
                        E<2>>>);
static_assert(same<brigand::push_back<Other<>,
                                      E<0>>,
                   Other<E<0>>>);
static_assert(same<brigand::wrap<list<E<0>,
                                      E<1>>,
                                 Other>,
                   Other<E<0>,
                         E<1>>>);
static_assert(same<brigand::wrap<Other<>,
                                 list>,
                   list<>>);

// append: none, one (any type), two, and 1..40 one-element lists (the 16-at-a-time path and its remainder)
static_assert(same<brigand::append<>,
                   list<>>);
static_assert(same<brigand::append<E<0>>,
                   E<0>>);
static_assert(same<brigand::append<list<>>,
                   list<>>);
static_assert(same<brigand::append<list<E<0>>,
                                   list<>,
                                   list<E<1>,
                                        E<2>>>,
                   list<E<0>,
                        E<1>,
                        E<2>>>);
static_assert(same<brigand::append<list<E<0>>,
                                   Other<E<1>>>,
                   list<E<0>,
                        E<1>>>);   // the first one's template
static_assert(same<brigand::append<Other<E<0>>,
                                   list<E<1>>>,
                   Other<E<0>,
                         E<1>>>);
static_assert(same<brigand::append<list<list<E<0>>>,
                                   list<list<E<1>>>>,
                   list<list<E<0>>,
                        list<E<1>>>>);

template<std::size_t... I>
constexpr bool appendOnes(std::index_sequence<I...>) {
    return same<brigand::append<list<E<static_cast<int>(I)>>...>, Iota<sizeof...(I)>>;
}

template<std::size_t... N>
constexpr bool appendSizes(std::index_sequence<N...>) {
    return (appendOnes(std::make_index_sequence<N + 1>{}) && ...);
}

static_assert(appendSizes(std::make_index_sequence<40>{}));

// flatten: nested lists of the same template are opened recursively, other templates stay elements
static_assert(same<brigand::flatten<list<>>,
                   list<>>);
static_assert(same<brigand::flatten<E<0>>,
                   E<0>>);
static_assert(same<brigand::flatten<list<E<0>,
                                         E<1>>>,
                   list<E<0>,
                        E<1>>>);
static_assert(same<brigand::flatten<list<list<E<0>,
                                              list<E<1>,
                                                   list<>>>,
                                         list<>,
                                         E<2>,
                                         list<list<list<E<3>>>>>>,
                   list<E<0>,
                        E<1>,
                        E<2>,
                        E<3>>>);
static_assert(same<brigand::flatten<list<Other<E<0>>,
                                         list<E<1>>>>,
                   list<Other<E<0>>,
                        E<1>>>);
static_assert(same<brigand::flatten<Other<Other<E<0>>,
                                          list<E<1>>>>,
                   Other<E<0>,
                         list<E<1>>>>);
static_assert(same<brigand::flatten<list<Iota<20>,
                                         Iota<20>>>,
                   brigand::append<Iota<20>,
                                   Iota<20>>>);
}   // namespace

int main() {
    // everything above is checked at compile time; reaching main means it all held
    Kvasir::Test::test("brigand_subset");
    return Kvasir::Test::report();
}
