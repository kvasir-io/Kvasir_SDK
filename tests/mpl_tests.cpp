// Tests for the Kvasir template metaprogramming layer: Mpl/Types.hpp, Mpl/Utility.hpp and
// Mpl/Algorithm.hpp.
//
// All of this is compile time only, so the test is a wall of static_asserts and main() just
// exists so ctest has something to run: if this file compiles, the tests passed.
#include "kvasir/Mpl/Algorithm.hpp"
#include "kvasir/Mpl/IntegralConstants.hpp"
#include "kvasir/Mpl/Types.hpp"
#include "kvasir/Mpl/Utility.hpp"
#include "test_harness.hpp"

#include <type_traits>

namespace mpl = Kvasir::MPL;

namespace {

template<typename A, typename B>
constexpr bool same = std::is_same_v<A, B>;

using mpl::Bool;
using mpl::FalseType;
using mpl::Int;
using mpl::TrueType;
using mpl::Unsigned;

template<typename... Ts>
using list = brigand::list<Ts...>;

// tag types used as list elements
struct A {};

struct B {};

struct C {};

struct D {};

// a predicate over a single type
template<typename T>
struct IsA : FalseType {};

template<>
struct IsA<A> : TrueType {};

using IsAP = mpl::Template<IsA>;

// a predicate that is true for every Int with an even value
template<typename T>
struct IsEven : FalseType {};

template<int I>
struct IsEven<Int<I>> : Bool<(I % 2 == 0)> {};

using IsEvenP = mpl::Template<IsEven>;

}   // namespace

// ===========================================================================
// Types.hpp
// ===========================================================================

static_assert(same<mpl::Return<A>::type,
                   A>);

static_assert(mpl::Value<int,
                         42>::value
              == 42);
static_assert(same<mpl::Value<int,
                              42>::type,
                   mpl::Value<int,
                              42>>);
static_assert(same<Int<7>,
                   mpl::Value<int,
                              7>>);
static_assert(same<Unsigned<7>,
                   mpl::Value<unsigned,
                              7>>);
static_assert(same<Bool<true>,
                   TrueType>);
static_assert(same<Bool<false>,
                   FalseType>);
static_assert(TrueType::value);
static_assert(!FalseType::value);

// Pair
static_assert(same<mpl::PairFirst<mpl::Pair<A,
                                            B>>,
                   A>);
static_assert(same<mpl::PairSecond<mpl::Pair<A,
                                             B>>,
                   B>);
static_assert(same<mpl::Pair<A,
                             B>::type,
                   mpl::Pair<A,
                             B>>);

// Template / ApplyTemplateT
static_assert(same<mpl::Template<IsA>::apply<A>,
                   IsA<A>>);
static_assert(same<mpl::ApplyTemplateT<IsAP,
                                       A>,
                   TrueType>);
static_assert(same<mpl::ApplyTemplateT<IsAP,
                                       B>,
                   FalseType>);

// ===========================================================================
// Utility.hpp
// ===========================================================================

// Not
static_assert(same<mpl::NotT<TrueType>,
                   FalseType>);
static_assert(same<mpl::NotT<FalseType>,
                   TrueType>);

// IsSame
static_assert(mpl::IsSame<A,
                          A>::value);
static_assert(!mpl::IsSame<A,
                           B>::value);
static_assert(same<mpl::IsSameT<A,
                                A>,
                   TrueType>);
static_assert(same<mpl::IsSameT<A,
                                B>,
                   FalseType>);

// Less, over Values of the same underlying type
static_assert(mpl::Less<Int<1>,
                        Int<2>>::value);
static_assert(!mpl::Less<Int<2>,
                         Int<1>>::value);
static_assert(!mpl::Less<Int<1>,
                         Int<1>>::value);
static_assert(mpl::Less<Unsigned<0>,
                        Unsigned<5>>::value);

// IsValue
static_assert(mpl::IsValue<Int<0>>::value);
static_assert(mpl::IsValue<Unsigned<3>>::value);
static_assert(mpl::IsValue<TrueType>::value);
static_assert(!mpl::IsValue<A>::value);
static_assert(!mpl::IsValue<list<>>::value);

// IsIntegral covers the built in integer types but not the tag types
static_assert(mpl::IsIntegral<int>::value);
static_assert(mpl::IsIntegral<unsigned>::value);
static_assert(mpl::IsIntegral<char>::value);
static_assert(mpl::IsIntegral<bool>::value);
static_assert(mpl::IsIntegral<long long>::value);
static_assert(mpl::IsIntegral<unsigned short>::value);
static_assert(!mpl::IsIntegral<float>::value);
static_assert(!mpl::IsIntegral<A>::value);
static_assert(!mpl::IsIntegral<int*>::value);
// note: cv qualified integers are not recognised, RemoveCVT has to be applied first
static_assert(!mpl::IsIntegral<int const>::value);
static_assert(mpl::IsIntegral<mpl::RemoveCVT<int const>>::value);

// RemoveConst / RemoveVolatile / RemoveCV
static_assert(same<mpl::RemoveConst<int const>::type,
                   int>);
static_assert(same<mpl::RemoveConst<int>::type,
                   int>);
static_assert(same<mpl::RemoveVolatile<int volatile>::type,
                   int>);
static_assert(same<mpl::RemoveCVT<int const volatile>,
                   int>);
static_assert(same<mpl::RemoveCVT<int>,
                   int>);

// EnableIf / DisableIf
static_assert(same<mpl::EnableIfT<true,
                                  A>,
                   A>);
static_assert(same<mpl::DisableIfT<false,
                                   A>,
                   A>);
static_assert(same<mpl::EnableIfT<true>,
                   void>);

// VoidT swallows any type
static_assert(same<mpl::VoidT<A>,
                   void>);
static_assert(same<mpl::VoidT<int>,
                   void>);

// AddRvalueReferenceT
static_assert(same<mpl::AddRvalueReference<int>::type,
                   int&&>);

// Size
static_assert(mpl::SizeT<list<>>::value == 0);
static_assert(mpl::SizeT<list<A>>::value == 1);
static_assert(mpl::SizeT<list<A,
                              B,
                              C>>::value
              == 3);

// Conditional
static_assert(same<mpl::ConditionalT<true,
                                     A,
                                     B>,
                   A>);
static_assert(same<mpl::ConditionalT<false,
                                     A,
                                     B>,
                   B>);

// BuildIndices
static_assert(same<mpl::BuildIndicesT<0>,
                   list<>>);
static_assert(same<mpl::BuildIndicesT<1>,
                   list<Int<0>>>);
static_assert(same<mpl::BuildIndicesT<4>,
                   list<Int<0>,
                        Int<1>,
                        Int<2>,
                        Int<3>>>);

// ===========================================================================
// Algorithm.hpp
// ===========================================================================

// --- At -------------------------------------------------------------------
static_assert(same<mpl::AtT<list<A,
                                 B,
                                 C>,
                            Int<0>>,
                   A>);
static_assert(same<mpl::AtT<list<A,
                                 B,
                                 C>,
                            Int<1>>,
                   B>);
static_assert(same<mpl::AtT<list<A,
                                 B,
                                 C>,
                            Int<2>>,
                   C>);

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

// Find with a predicate
static_assert(mpl::FindT<list<A,
                              B,
                              C>,
                         IsAP>::value
              == 0);
static_assert(mpl::FindT<list<B,
                              A,
                              C>,
                         IsAP>::value
              == 1);
static_assert(mpl::FindT<list<B,
                              C>,
                         IsAP>::value
              == -1);

// --- Contains -------------------------------------------------------------
static_assert(mpl::ContainsT<list<A,
                                  B,
                                  C>,
                             A>::value);
static_assert(mpl::ContainsT<list<A,
                                  B,
                                  C>,
                             C>::value);
static_assert(!mpl::ContainsT<list<A,
                                   B,
                                   C>,
                              D>::value);
static_assert(!mpl::ContainsT<list<>,
                              A>::value);

// --- Get ------------------------------------------------------------------
static_assert(same<mpl::GetT<list<A,
                                  B,
                                  C>,
                             IsAP>,
                   A>);
static_assert(same<mpl::GetT<list<B,
                                  A,
                                  C>,
                             IsAP>,
                   A>);
// no match returns the default, which is void unless given
static_assert(same<mpl::GetT<list<B,
                                  C>,
                             IsAP>,
                   void>);
static_assert(same<mpl::GetT<list<B,
                                  C>,
                             IsAP,
                             D>,
                   D>);

// --- Sum ------------------------------------------------------------------
static_assert(mpl::SumT<list<>>::value == 0);
static_assert(mpl::SumT<list<Int<1>>>::value == 1);
static_assert(mpl::SumT<list<Int<1>,
                             Int<2>,
                             Int<3>>>::value
              == 6);
static_assert(mpl::SumT<list<Int<-1>,
                             Int<1>>>::value
              == 0);
static_assert(mpl::SumT<list<TrueType,
                             FalseType,
                             TrueType>>::value
              == 2);

// --- CountIf --------------------------------------------------------------
static_assert(mpl::CountIfT<list<>,
                            IsAP>::value
              == 0);
static_assert(mpl::CountIfT<list<A,
                                 B,
                                 C>,
                            IsAP>::value
              == 1);
static_assert(mpl::CountIfT<list<A,
                                 A,
                                 B>,
                            IsAP>::value
              == 2);
static_assert(mpl::CountIfT<list<B,
                                 C>,
                            IsAP>::value
              == 0);
static_assert(mpl::CountIfT<list<Int<1>,
                                 Int<2>,
                                 Int<3>,
                                 Int<4>>,
                            IsEvenP>::value
              == 2);

// --- Sort -----------------------------------------------------------------
static_assert(same<mpl::SortT<list<>>,
                   list<>>);
static_assert(same<mpl::SortT<list<Int<1>>>,
                   list<Int<1>>>);
static_assert(same<mpl::SortT<list<Int<2>,
                                   Int<1>>>,
                   list<Int<1>,
                        Int<2>>>);
static_assert(same<mpl::SortT<list<Int<1>,
                                   Int<2>>>,
                   list<Int<1>,
                        Int<2>>>);
static_assert(same<mpl::SortT<list<Int<3>,
                                   Int<1>,
                                   Int<2>>>,
                   list<Int<1>,
                        Int<2>,
                        Int<3>>>);
static_assert(same<mpl::SortT<list<Int<5>,
                                   Int<4>,
                                   Int<3>,
                                   Int<2>,
                                   Int<1>>>,
                   list<Int<1>,
                        Int<2>,
                        Int<3>,
                        Int<4>,
                        Int<5>>>);
// an already sorted list is unchanged
static_assert(same<mpl::SortT<list<Int<1>,
                                   Int<2>,
                                   Int<3>>>,
                   list<Int<1>,
                        Int<2>,
                        Int<3>>>);
// duplicates are kept
static_assert(same<mpl::SortT<list<Int<2>,
                                   Int<1>,
                                   Int<2>>>,
                   list<Int<1>,
                        Int<2>,
                        Int<2>>>);
// unsigned values sort too
static_assert(same<mpl::SortT<list<Unsigned<3>,
                                   Unsigned<1>>>,
                   list<Unsigned<1>,
                        Unsigned<3>>>);

// --- Unique ---------------------------------------------------------------
// Unique expects a sorted list and drops consecutive duplicates
static_assert(same<mpl::UniqueT<list<>>,
                   list<>>);
static_assert(same<mpl::UniqueT<list<A>>,
                   list<A>>);
static_assert(same<mpl::UniqueT<list<A,
                                     A>>,
                   list<A>>);
static_assert(same<mpl::UniqueT<list<A,
                                     B>>,
                   list<A,
                        B>>);
static_assert(same<mpl::UniqueT<list<A,
                                     A,
                                     B>>,
                   list<A,
                        B>>);
static_assert(same<mpl::UniqueT<list<A,
                                     B,
                                     B>>,
                   list<A,
                        B>>);
static_assert(same<mpl::UniqueT<list<A,
                                     A,
                                     B,
                                     B,
                                     C>>,
                   list<A,
                        B,
                        C>>);
static_assert(same<mpl::UniqueT<list<A,
                                     A,
                                     A>>,
                   list<A>>);
static_assert(same<mpl::UniqueT<list<A,
                                     B,
                                     C>>,
                   list<A,
                        B,
                        C>>);
// only *consecutive* duplicates are removed, which is why the input has to be sorted
static_assert(same<mpl::UniqueT<list<A,
                                     B,
                                     A>>,
                   list<A,
                        B,
                        A>>);

// sort then unique, the combination the Io layer uses to collect port numbers
static_assert(same<mpl::UniqueT<mpl::SortT<list<Int<3>,
                                                Int<1>,
                                                Int<3>,
                                                Int<1>>>>,
                   list<Int<1>,
                        Int<3>>>);

// --- Remove ---------------------------------------------------------------
// index based removal, [From, To)
static_assert(same<mpl::RemoveT<list<A,
                                     B,
                                     C,
                                     D>,
                                Int<0>,
                                Int<1>>,
                   list<B,
                        C,
                        D>>);
static_assert(same<mpl::RemoveT<list<A,
                                     B,
                                     C,
                                     D>,
                                Int<1>,
                                Int<2>>,
                   list<A,
                        C,
                        D>>);
static_assert(same<mpl::RemoveT<list<A,
                                     B,
                                     C,
                                     D>,
                                Int<1>,
                                Int<3>>,
                   list<A,
                        D>>);
static_assert(same<mpl::RemoveT<list<A,
                                     B,
                                     C,
                                     D>,
                                Int<0>,
                                Int<4>>,
                   list<>>);
// an empty range removes nothing
static_assert(same<mpl::RemoveT<list<A,
                                     B,
                                     C>,
                                Int<1>,
                                Int<1>>,
                   list<A,
                        B,
                        C>>);

// predicate based removal
static_assert(same<mpl::RemoveT<list<A,
                                     B,
                                     C>,
                                IsAP>,
                   list<B,
                        C>>);
static_assert(same<mpl::RemoveT<list<A,
                                     A,
                                     B>,
                                IsAP>,
                   list<B>>);
static_assert(same<mpl::RemoveT<list<B,
                                     C>,
                                IsAP>,
                   list<B,
                        C>>);
static_assert(same<mpl::RemoveT<list<A>,
                                IsAP>,
                   list<>>);
static_assert(same<mpl::RemoveT<list<Int<1>,
                                     Int<2>,
                                     Int<3>,
                                     Int<4>>,
                                IsEvenP>,
                   list<Int<1>,
                        Int<3>>>);

// --- RepeatC --------------------------------------------------------------
static_assert(same<mpl::RepeatC<0,
                                A>,
                   list<>>);
static_assert(same<mpl::RepeatC<1,
                                A>,
                   list<A>>);
static_assert(same<mpl::RepeatC<2,
                                A>,
                   list<A,
                        A>>);
static_assert(same<mpl::RepeatC<3,
                                A>,
                   list<A,
                        A,
                        A>>);
static_assert(same<mpl::RepeatC<5,
                                A>,
                   list<A,
                        A,
                        A,
                        A,
                        A>>);
static_assert(mpl::SizeT<mpl::RepeatC<17,
                                      A>>::value
              == 17);
// repeating a pack repeats the whole pack
static_assert(same<mpl::RepeatC<2,
                                A,
                                B>,
                   list<A,
                        B,
                        A,
                        B>>);

// --- Detail::Join / Detail::Split -----------------------------------------
// Join concatenates lists, putting the delimiter between the original elements
static_assert(same<typename mpl::Detail::Join<list<>,
                                              D,
                                              list<A>,
                                              list<B>>::type,
                   list<D,
                        A,
                        D,
                        B>>);
static_assert(same<typename mpl::Detail::Join<list<>,
                                              D,
                                              list<A,
                                                   B>>::type,
                   list<D,
                        A,
                        B>>);
static_assert(same<typename mpl::Detail::Join<list<>,
                                              D>::type,
                   list<>>);

// Split cuts a flat list into sublists at every delimiter
static_assert(same<typename mpl::Detail::Split<list<>,
                                               list<>,
                                               D,
                                               A,
                                               B>::type,
                   list<list<A,
                             B>>>);
static_assert(same<typename mpl::Detail::Split<list<>,
                                               list<>,
                                               D,
                                               A,
                                               D,
                                               B>::type,
                   list<list<A>,
                        list<B>>>);
static_assert(same<typename mpl::Detail::Split<list<>,
                                               list<>,
                                               D,
                                               A,
                                               D,
                                               B,
                                               D,
                                               C>::type,
                   list<list<A>,
                        list<B>,
                        list<C>>>);

// ===========================================================================
// IntegralConstants.hpp
// ===========================================================================
namespace {
using namespace Kvasir::MPL::IntegralConstants;

// digits compose left to right through the comma operator
static_assert(same<decltype((_1,
                             _2)),
                   Unsigned<12>>);
static_assert(same<decltype((_1,
                             _2,
                             _3)),
                   Unsigned<123>>);
static_assert(same<decltype((_4,
                             _0)),
                   Unsigned<40>>);
static_assert(same<decltype((_0,
                             _7)),
                   Unsigned<7>>);
static_assert(decltype((_9,
                        _8,
                        _7))::value
              == 987);
}   // namespace

int main() {
    // everything above is checked at compile time; reaching main means it all held
    Kvasir::Test::test("mpl");
    return Kvasir::Test::report();
}
