// Tests for kvasir/Mpl/List.hpp: the fold-based list helpers of the Startup headers.
//
// Compile time only: if this file compiles, the tests passed; main() exists for ctest.
#include "kvasir/Mpl/List.hpp"
#include "test_harness.hpp"

#include <type_traits>

namespace mpl = Kvasir::MPL;

namespace {

template<typename A, typename B>
constexpr bool same = std::is_same_v<A, B>;

template<typename... Ts>
using list = brigand::list<Ts...>;

struct A {};

struct B {};

struct C {};

template<typename T>
struct IsA : std::is_same<T, A> {};

struct IsAOrB {
    template<typename T>
    static constexpr bool holds = std::is_same_v<T, A> || std::is_same_v<T, B>;
};

// a predicate with a bound parameter, the form the Startup rules use
template<typename Of>
struct SameAs {
    template<typename T>
    static constexpr bool holds = std::is_same_v<T, Of>;
};

// ---- count / contains ----------------------------------------------------------------
static_assert(mpl::count<A,
                         list<>>
              == 0);
static_assert(mpl::count<A,
                         list<B,
                              C>>
              == 0);
static_assert(mpl::count<A,
                         list<A,
                              B,
                              A,
                              C,
                              A>>
              == 3);
static_assert(!mpl::contains<A,
                             list<>>);
static_assert(!mpl::contains<A,
                             list<B,
                                  C>>);
static_assert(mpl::contains<C,
                            list<A,
                                 B,
                                 C>>);

// ---- Dedup: the FIRST occurrence kept, order kept ---------------------------------------
static_assert(same<mpl::Dedup<list<>>,
                   list<>>);
static_assert(same<mpl::Dedup<list<A>>,
                   list<A>>);
static_assert(same<mpl::Dedup<list<A,
                                   B,
                                   A,
                                   C,
                                   B>>,
                   list<A,
                        B,
                        C>>);
// a non-adjacent repeat goes too
static_assert(same<mpl::Dedup<list<A,
                                   B,
                                   A>>,
                   list<A,
                        B>>);
static_assert(same<mpl::Dedup<list<C,
                                   C,
                                   B,
                                   A,
                                   B>>,
                   list<C,
                        B,
                        A>>);

// ---- Filter, with Holds and NotP --------------------------------------------------------
static_assert(same<mpl::Filter<mpl::Holds<IsA>,
                               list<>>,
                   list<>>);
static_assert(same<mpl::Filter<mpl::Holds<IsA>,
                               list<B,
                                    A,
                                    C,
                                    A>>,
                   list<A,
                        A>>);
static_assert(same<mpl::Filter<mpl::NotP<mpl::Holds<IsA>>,
                               list<B,
                                    A,
                                    C,
                                    A>>,
                   list<B,
                        C>>);
static_assert(same<mpl::Filter<IsAOrB,
                               list<C,
                                    B,
                                    A,
                                    C>>,
                   list<B,
                        A>>);
static_assert(same<mpl::Filter<mpl::NotP<IsAOrB>,
                               list<C,
                                    B,
                                    A,
                                    C>>,
                   list<C,
                        C>>);
static_assert(same<mpl::Filter<SameAs<C>,
                               list<A,
                                    B>>,
                   list<>>);

// ---- FirstWhere: the first of several matches, void when none -----------------------------
static_assert(same<mpl::FirstWhereT<IsAOrB>,
                   void>);
static_assert(same<mpl::FirstWhereT<IsAOrB,
                                    C>,
                   void>);
static_assert(same<mpl::FirstWhereT<IsAOrB,
                                    C,
                                    B,
                                    A>,
                   B>);
static_assert(same<mpl::FirstWhereT<IsAOrB,
                                    A,
                                    B>,
                   A>);
static_assert(same<typename mpl::FirstWhere<SameAs<C>,
                                            A,
                                            C,
                                            C>::type,
                   C>);

// ---- Expand ------------------------------------------------------------------------------
template<typename... Ts>
struct Pack {};

static_assert(same<mpl::Expand<Pack,
                               list<>>,
                   Pack<>>);
static_assert(same<mpl::Expand<Pack,
                               list<A,
                                    B>>,
                   Pack<A,
                        B>>);

// a template with a fixed leading parameter, bound through a member alias template
template<int N, typename... Ts>
struct Fixed {};

template<int N>
struct Bind {
    template<typename... Ts>
    using type = Fixed<N, Ts...>;
};

static_assert(same<mpl::Expand<Bind<7>::template type,
                               list<A,
                                    C>>,
                   Fixed<7,
                         A,
                         C>>);
}   // namespace

int main() {
    // everything above is checked at compile time; reaching main means it all held
    Kvasir::Test::test("mpl_list");
    return Kvasir::Test::report();
}
