// Tests for the containers layered on top of StaticVector: StaticString, StaticMap and
// StaticSet.
#include "kvasir/Util/StaticMap.hpp"
#include "kvasir/Util/StaticSet.hpp"
#include "kvasir/Util/StaticString.hpp"
#include "test_harness.hpp"

#include <algorithm>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

using Kvasir::StaticMap;
using Kvasir::StaticSet;
using Kvasir::StaticString;

// ===========================================================================
// StaticString
// ===========================================================================

static_assert(StaticString<8>{}.max_size() == 8);
static_assert(StaticString<8>{}.capacity() == 8);
static_assert(StaticString<8>{}.empty());

// construction and conversion happen at compile time
static_assert(std::string_view{StaticString<8>{"abc"}} == "abc");
static_assert(StaticString<8>{"abc"}.size() == 3);
static_assert(StaticString<3>{"abc"}.full());

static void stringConstruction() {
    Kvasir::Test::test("stringConstruction");

    StaticString<16> empty;
    CHECK(empty.empty());
    CHECK_EQ(empty.size(), 0U);
    CHECK_EQ(empty.capacity(), 16U);
    CHECK(!empty.full());

    // from a string literal
    StaticString<16> fromLiteral{"hello"};
    CHECK_EQ(fromLiteral.size(), 5U);
    CHECK(std::string_view{fromLiteral} == "hello");

    // from a string_view
    StaticString<16> fromView{std::string_view{"world"}};
    CHECK(std::string_view{fromView} == "world");

    // copy construction from a differently sized StaticString
    StaticString<32> wider{fromLiteral};
    CHECK(std::string_view{wider} == "hello");
    CHECK_EQ(wider.capacity(), 32U);

    // exactly at capacity
    StaticString<5> exact{"hello"};
    CHECK(exact.full());
    CHECK(std::string_view{exact} == "hello");

    // the empty string
    StaticString<4> nothing{""};
    CHECK(nothing.empty());
}

static void stringAssignment() {
    Kvasir::Test::test("stringAssignment");

    StaticString<16> s{"initial"};

    s = "replaced";
    CHECK(std::string_view{s} == "replaced");

    s = std::string_view{"view"};
    CHECK(std::string_view{s} == "view");

    StaticString<8> other{"other"};
    s = other;
    CHECK(std::string_view{s} == "other");

    // assigning over a longer value must shorten the string, not leave a tail
    s = "ab";
    CHECK_EQ(s.size(), 2U);
    CHECK(std::string_view{s} == "ab");

    // assigning the empty string
    s = "";
    CHECK(s.empty());
}

static void stringElementAccess() {
    Kvasir::Test::test("stringElementAccess");

    StaticString<16> s{"abcde"};

    CHECK(s.front() == 'a');
    CHECK(s.back() == 'e');
    CHECK(s.data()[0] == 'a');

    auto const& cs = s;
    CHECK(cs.front() == 'a');
    CHECK(cs.back() == 'e');
    CHECK(cs.data()[4] == 'e');

    s.front() = 'A';
    s.back()  = 'E';
    CHECK(std::string_view{s} == "AbcdE");

    // iteration, forwards and backwards
    std::string_view const expected{"AbcdE"};
    CHECK(std::equal(s.begin(), s.end(), expected.begin(), expected.end()));

    std::vector<char> reversed{s.rbegin(), s.rend()};
    CHECK((reversed == std::vector<char>{'E', 'd', 'c', 'b', 'A'}));

    CHECK(std::equal(cs.begin(), cs.end(), expected.begin(), expected.end()));
    CHECK(std::distance(cs.rbegin(), cs.rend()) == 5);
}

static void stringModifiers() {
    Kvasir::Test::test("stringModifiers");

    StaticString<16> s;
    s.push_back('a');
    s.push_back('b');
    s.push_back('c');
    CHECK(std::string_view{s} == "abc");

    // resize truncates
    s.resize(2);
    CHECK(std::string_view{s} == "ab");

    // resize pads with '\0'
    s.resize(4);
    CHECK_EQ(s.size(), 4U);
    CHECK(s.data()[2] == '\0');
    CHECK(s.data()[3] == '\0');

    s       = "abcdef";
    auto it = s.erase(s.begin() + 1, s.begin() + 3);
    CHECK(std::string_view{s} == "adef");
    CHECK(it == s.begin() + 1);

    // erasing an empty range changes nothing
    s.erase(s.begin(), s.begin());
    CHECK(std::string_view{s} == "adef");

    s.clear();
    CHECK(s.empty());
}

static void stringOperations() {
    Kvasir::Test::test("stringOperations");

    StaticString<16> s{"prefix_body"};

    CHECK(s.starts_with("prefix"));
    CHECK(s.starts_with("prefix_body"));
    CHECK(s.starts_with(""));
    CHECK(!s.starts_with("prefixx"));
    CHECK(!s.starts_with("body"));
    // a needle longer than the string can never match
    CHECK(!s.starts_with("prefix_body_and_more"));

    CHECK(s.starts_with('p'));
    CHECK(!s.starts_with('x'));

    StaticString<16> const empty;
    CHECK(!empty.starts_with('x'));
    CHECK(empty.starts_with(""));

    // concatenation
    StaticString<16> a{"foo"};
    auto const       joined = a + "bar";
    CHECK(std::string_view{joined} == "foobar");
    CHECK(std::string_view{a} == "foo");   // the original is unchanged

    auto const empties = StaticString<8>{""} + "";
    CHECK(empties.empty());
}

static void stringComparison() {
    Kvasir::Test::test("stringComparison");

    StaticString<16> a{"abc"};
    StaticString<16> b{"abc"};
    StaticString<8>  narrow{"abc"};
    StaticString<16> different{"abd"};

    CHECK(a == b);
    CHECK(a == narrow);   // capacity does not take part in the comparison
    CHECK(!(a == different));

    CHECK(a == "abc");
    CHECK(!(a == "abcd"));
    CHECK(!(a == "ab"));

    CHECK(a == std::string_view{"abc"});
    CHECK(!(a == std::string_view{"abd"}));

    CHECK(StaticString<4>{} == "");
}

// operator""_ss is declared as `template<char... chars>`, which the language only uses for
// user defined *numeric* literals -- `"text"_ss` does not compile. Building a StaticString
// from a string literal therefore goes through the constructor; the literal operator only
// works on digit sequences.
static void stringLiteral() {
    Kvasir::Test::test("stringLiteral");

    using namespace Kvasir;

    auto const s = 12345_ss;
    CHECK(std::string_view{s} == "12345");
    CHECK_EQ(s.size(), 5U);
    CHECK_EQ(s.capacity(), 5U);
    CHECK(s.full());

    // digit separators are part of the literal's character pack and are kept verbatim
    auto const separated = 1'2_ss;
    CHECK(std::string_view{separated} == "1'2");
}

// ===========================================================================
// StaticMap
// ===========================================================================

static_assert(StaticMap<int,
                        int,
                        4>::capacity()
              == 4);
static_assert(StaticMap<int,
                        int,
                        4>::max_size()
              == 4);

// Note: StaticMap is not usable in a constant expression. Its element type is
// std::pair<Key, Value>, which is trivially copyable but not trivially *default*
// constructible, so StaticVector selects the non_trivial storage backend -- and that
// backend manages lifetimes with placement new and explicit destructor calls, neither of
// which is allowed during constant evaluation. StaticSet<int, N> stores plain ints and so
// does stay constexpr usable (see below).

static void mapInsertAndLookup() {
    Kvasir::Test::test("mapInsertAndLookup");

    StaticMap<int, int, 4> m;
    CHECK(m.empty());
    CHECK(!m.full());
    CHECK_EQ(m.size(), 0U);

    auto [it1, inserted1] = m.insert({1, 10});
    CHECK(inserted1);
    CHECK(it1->first == 1);
    CHECK(it1->second == 10);
    CHECK_EQ(m.size(), 1U);

    // inserting a duplicate key does nothing and reports the existing element
    auto [it2, inserted2] = m.insert({1, 99});
    CHECK(!inserted2);
    CHECK(it2 == it1);
    CHECK(it2->second == 10);   // the original value survives
    CHECK_EQ(m.size(), 1U);

    m.insert({2, 20});
    m.insert({3, 30});
    CHECK_EQ(m.size(), 3U);

    CHECK(m.contains(1));
    CHECK(m.contains(3));
    CHECK(!m.contains(4));

    CHECK(m.find(2) != m.end());
    CHECK(m.find(2)->second == 20);
    CHECK(m.find(4) == m.end());

    CHECK(m.at(1) == 10);
    CHECK(m.at(2) == 20);

    auto const& cm = m;
    CHECK(cm.at(3) == 30);
    CHECK(cm.find(3) != cm.end());
    CHECK(cm.find(9) == cm.end());
    CHECK(cm.contains(2));

    // insertion order is preserved
    std::vector<int> keys;
    for(auto const& kv : m) { keys.push_back(kv.first); }
    CHECK((keys == std::vector<int>{1, 2, 3}));

    // filling to capacity
    m.insert({4, 40});
    CHECK(m.full());
    CHECK_EQ(m.size(), 4U);
    // a duplicate insert on a full map is still fine, it never touches the storage
    auto [it3, inserted3] = m.insert({1, 0});
    CHECK(!inserted3);
    CHECK(it3->second == 10);
}

static void mapInsertOrAssignAndSubscript() {
    Kvasir::Test::test("mapInsertOrAssignAndSubscript");

    StaticMap<int, int, 4> m;

    auto [it1, inserted1] = m.insert_or_assign(1, 10);
    CHECK(inserted1);
    CHECK(it1->second == 10);

    // the same key again assigns rather than inserts
    auto [it2, inserted2] = m.insert_or_assign(1, 11);
    CHECK(!inserted2);
    CHECK(it2 == it1);
    CHECK(it2->second == 11);
    CHECK_EQ(m.size(), 1U);

    // operator[] default constructs a missing value
    CHECK(m[2] == 0);
    CHECK_EQ(m.size(), 2U);
    m[2] = 20;
    CHECK(m.at(2) == 20);

    // operator[] on an existing key returns a reference to the stored value
    m[1] = 111;
    CHECK(m.at(1) == 111);
    CHECK_EQ(m.size(), 2U);

    // insert_unchecked skips the duplicate search
    m.insert_unchecked(5, 50);
    CHECK(m.at(5) == 50);
    CHECK_EQ(m.size(), 3U);
}

static void mapErase() {
    Kvasir::Test::test("mapErase");

    StaticMap<int, int, 8> m{
      {1, 10},
      {2, 20},
      {3, 30},
      {4, 40}
    };
    CHECK_EQ(m.size(), 4U);

    CHECK_EQ(m.erase(2), 1U);
    CHECK_EQ(m.size(), 3U);
    CHECK(!m.contains(2));

    // erasing a key that is not there reports 0 and changes nothing
    CHECK_EQ(m.erase(99), 0U);
    CHECK_EQ(m.size(), 3U);

    // the remaining elements keep their relative order
    std::vector<int> keys;
    for(auto const& kv : m) { keys.push_back(kv.first); }
    CHECK((keys == std::vector<int>{1, 3, 4}));

    // erase by iterator returns the following element
    auto next = m.erase(m.begin());
    CHECK(next == m.begin());
    CHECK(next->first == 3);
    CHECK_EQ(m.size(), 2U);

    m.clear();
    CHECK(m.empty());
    CHECK_EQ(m.size(), 0U);
    CHECK(!m.contains(3));
}

static void mapConstructionAndEquality() {
    Kvasir::Test::test("mapConstructionAndEquality");

    // duplicate keys in the initializer list: the first one wins
    StaticMap<int, int, 4> withDuplicates{
      {1, 10},
      {1, 99},
      {2, 20}
    };
    CHECK_EQ(withDuplicates.size(), 2U);
    CHECK(withDuplicates.at(1) == 10);

    StaticMap<int, int, 4> a{
      {1, 10},
      {2, 20}
    };
    StaticMap<int, int, 4> sameContentDifferentOrder{
      {2, 20},
      {1, 10}
    };
    StaticMap<int, int, 4> differentValue{
      {1, 10},
      {2, 21}
    };
    StaticMap<int, int, 4> differentKey{
      {1, 10},
      {3, 20}
    };
    StaticMap<int, int, 4> shorter{
      {1, 10}
    };

    // equality is order independent
    CHECK(a == sameContentDifferentOrder);
    CHECK(!(a != sameContentDifferentOrder));
    CHECK(a != differentValue);
    CHECK(a != differentKey);
    CHECK(a != shorter);

    // copy and move
    StaticMap<int, int, 4> copy{a};
    CHECK(copy == a);

    StaticMap<int, int, 4> assigned;
    assigned = a;
    CHECK(assigned == a);

    StaticMap<int, int, 4> moved{std::move(copy)};
    CHECK(moved == a);

    StaticMap<int, int, 4> moveAssigned;
    moveAssigned = std::move(assigned);
    CHECK(moveAssigned == a);

    StaticMap<int, int, 4> const empty1;
    StaticMap<int, int, 4> const empty2;
    CHECK(empty1 == empty2);
    CHECK(empty1 != a);
}

// a key type that is only equality comparable, which is all StaticMap requires
static void mapWithNonTrivialKey() {
    Kvasir::Test::test("mapWithNonTrivialKey");

    StaticMap<StaticString<8>, int, 4> m;
    m.insert({StaticString<8>{"one"}, 1});
    m.insert({StaticString<8>{"two"}, 2});

    CHECK(m.contains(StaticString<8>{"one"}));
    CHECK(!m.contains(StaticString<8>{"three"}));
    CHECK(m.at(StaticString<8>{"two"}) == 2);
    CHECK_EQ(m.size(), 2U);
}

// ===========================================================================
// StaticSet
// ===========================================================================

static_assert(StaticSet<int,
                        4>::capacity()
              == 4);
static_assert(StaticSet<int,
                        4>::max_size()
              == 4);

static_assert([] {
    StaticSet<int, 4> s{1, 2, 3};
    s.insert(2);
    return s.size() == 3 && s.contains(1) && !s.contains(9);
}());

static void setInsertAndLookup() {
    Kvasir::Test::test("setInsertAndLookup");

    StaticSet<int, 4> s;
    CHECK(s.empty());
    CHECK(!s.full());

    auto [it1, inserted1] = s.insert(1);
    CHECK(inserted1);
    CHECK(*it1 == 1);
    CHECK_EQ(s.size(), 1U);

    // a duplicate is rejected and the existing element reported
    auto [it2, inserted2] = s.insert(1);
    CHECK(!inserted2);
    CHECK(it2 == it1);
    CHECK_EQ(s.size(), 1U);

    s.insert(2);
    s.insert(3);
    CHECK_EQ(s.size(), 3U);

    CHECK(s.contains(1));
    CHECK(s.contains(3));
    CHECK(!s.contains(4));
    CHECK(s.find(2) != s.end());
    CHECK(*s.find(2) == 2);
    CHECK(s.find(4) == s.end());

    auto const& cs = s;
    CHECK(cs.find(1) != cs.end());
    CHECK(cs.find(9) == cs.end());
    CHECK(cs.contains(2));

    // insertion order is preserved
    std::vector<int> values{s.begin(), s.end()};
    CHECK((values == std::vector<int>{1, 2, 3}));

    s.insert(4);
    CHECK(s.full());

    // insert_unchecked skips the duplicate search
    StaticSet<int, 4> u;
    u.insert_unchecked(7);
    u.insert_unchecked(8);
    CHECK(u.contains(7));
    CHECK(u.contains(8));
    CHECK_EQ(u.size(), 2U);
}

static void setErase() {
    Kvasir::Test::test("setErase");

    StaticSet<int, 8> s{1, 2, 3, 4};

    CHECK_EQ(s.erase(2), 1U);
    CHECK_EQ(s.size(), 3U);
    CHECK(!s.contains(2));

    CHECK_EQ(s.erase(99), 0U);
    CHECK_EQ(s.size(), 3U);

    std::vector<int> values{s.begin(), s.end()};
    CHECK((values == std::vector<int>{1, 3, 4}));

    auto next = s.erase(s.begin());
    CHECK(next == s.begin());
    CHECK(*next == 3);

    s.clear();
    CHECK(s.empty());
}

static void setConstructionAndEquality() {
    Kvasir::Test::test("setConstructionAndEquality");

    // duplicates in the initializer list collapse
    StaticSet<int, 4> withDuplicates{1, 1, 2};
    CHECK_EQ(withDuplicates.size(), 2U);

    StaticSet<int, 4> a{1, 2};
    StaticSet<int, 4> reordered{2, 1};
    StaticSet<int, 4> different{1, 3};
    StaticSet<int, 4> shorter{1};

    CHECK(a == reordered);   // equality is order independent
    CHECK(!(a != reordered));
    CHECK(a != different);
    CHECK(a != shorter);

    StaticSet<int, 4> copy{a};
    CHECK(copy == a);

    StaticSet<int, 4> assigned;
    assigned = a;
    CHECK(assigned == a);

    StaticSet<int, 4> moved{std::move(copy)};
    CHECK(moved == a);

    StaticSet<int, 4> const empty1;
    StaticSet<int, 4> const empty2;
    CHECK(empty1 == empty2);
    CHECK(empty1 != a);
}

int main() {
    stringConstruction();
    stringAssignment();
    stringElementAccess();
    stringModifiers();
    stringOperations();
    stringComparison();
    stringLiteral();

    mapInsertAndLookup();
    mapInsertOrAssignAndSubscript();
    mapErase();
    mapConstructionAndEquality();
    mapWithNonTrivialKey();

    setInsertAndLookup();
    setErase();
    setConstructionAndEquality();

    return Kvasir::Test::report();
}
