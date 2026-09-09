// Tests for Kvasir::StaticVector.
//
// StaticVector picks one of three storage backends depending on T (zero_sized for
// Capacity 0, trivial for trivially destructible T and non_trivial otherwise), and the
// non_trivial one manages object lifetimes by hand. The tests therefore run the same
// scenarios over a trivial element type and over a type that counts its own constructions
// and destructions, so that a missed destructor call shows up as a leak.
#include "kvasir/Util/StaticVector.hpp"
#include "test_harness.hpp"

#include <algorithm>
#include <array>
#include <iterator>
#include <numeric>
#include <type_traits>
#include <utility>
#include <vector>

using Kvasir::StaticVector;

namespace {

// ---------------------------------------------------------------------------
// an element type that is not trivially destructible and keeps a ledger of every
// construction and destruction, so leaks and double destroys are detectable
struct Tracked {
    static inline int constructed = 0;
    static inline int destroyed   = 0;
    static inline int copies      = 0;
    static inline int moves       = 0;

    static void reset() {
        constructed = 0;
        destroyed   = 0;
        copies      = 0;
        moves       = 0;
    }

    static int alive() { return constructed - destroyed; }

    int value;

    Tracked() : value{0} { ++constructed; }

    explicit Tracked(int v) : value{v} { ++constructed; }

    Tracked(Tracked const& other) : value{other.value} {
        ++constructed;
        ++copies;
    }

    Tracked(Tracked&& other) noexcept : value{other.value} {
        other.value = -1;
        ++constructed;
        ++moves;
    }

    Tracked& operator=(Tracked const& other) {
        value = other.value;
        ++copies;
        return *this;
    }

    Tracked& operator=(Tracked&& other) noexcept {
        value       = other.value;
        other.value = -1;
        ++moves;
        return *this;
    }

    ~Tracked() { ++destroyed; }

    bool operator==(Tracked const& other) const { return value == other.value; }

    auto operator<=>(Tracked const& other) const { return value <=> other.value; }
};

static_assert(!std::is_trivially_destructible_v<Tracked>);
static_assert(std::is_trivially_destructible_v<int>);

// contents of a vector as a plain std::vector<int>, for readable comparisons
template<typename V>
std::vector<int> contents(V const& v) {
    std::vector<int> out;
    for(auto const& e : v) {
        if constexpr(std::is_same_v<std::decay_t<decltype(e)>, Tracked>) {
            out.push_back(e.value);
        } else {
            out.push_back(static_cast<int>(e));
        }
    }
    return out;
}

template<typename T>
T make(int v) {
    if constexpr(std::is_same_v<T, Tracked>) {
        return Tracked{v};
    } else {
        return static_cast<T>(v);
    }
}

}   // namespace

// ---------------------------------------------------------------------------
// compile time properties
static_assert(StaticVector<int,
                           8>::capacity()
              == 8);
static_assert(StaticVector<int,
                           8>::max_size()
              == 8);
static_assert(StaticVector<int,
                           0>::capacity()
              == 0);
static_assert(std::is_same_v<StaticVector<int,
                                          8>::value_type,
                             int>);
static_assert(std::is_same_v<StaticVector<int,
                                          8>::iterator,
                             int*>);
static_assert(std::is_same_v<StaticVector<int,
                                          8>::const_iterator,
                             int const*>);

// the whole container works in a constant expression for a trivial element type
static_assert([] {
    StaticVector<int, 8> v{1, 2, 3};
    v.push_back(4);
    v.insert(v.begin() + 1, 99);
    v.erase(v.begin());
    return v.size() == 4 && v[0] == 99 && v[1] == 2 && v[2] == 3 && v[3] == 4;
}());

static_assert([] {
    StaticVector<int, 4> v(3u, 7);
    v.resize(1);
    v.resize(4, 5);
    return v.size() == 4 && v[0] == 7 && v[1] == 5 && v[2] == 5 && v[3] == 5;
}());

// ---------------------------------------------------------------------------
template<typename T>
static void emptyState(char const* name) {
    Kvasir::Test::test(name);

    StaticVector<T, 4> v;
    CHECK(v.empty());
    CHECK(!v.full());
    CHECK_EQ(v.size(), 0U);
    CHECK_EQ(v.capacity(), 4U);
    CHECK(v.begin() == v.end());
    CHECK(v.cbegin() == v.cend());
    CHECK(contents(v).empty());
}

template<typename T>
static void pushPopBack(char const* name) {
    Kvasir::Test::test(name);

    StaticVector<T, 4> v;
    v.push_back(make<T>(1));
    CHECK_EQ(v.size(), 1U);
    CHECK(!v.empty());
    CHECK(contents(v) == std::vector<int>{1});

    v.push_back(make<T>(2));
    v.push_back(make<T>(3));
    CHECK(contents(v) == (std::vector<int>{1, 2, 3}));
    CHECK(!v.full());

    v.push_back(make<T>(4));
    CHECK(v.full());
    CHECK_EQ(v.size(), 4U);

    v.pop_back();
    CHECK_EQ(v.size(), 3U);
    CHECK(!v.full());
    CHECK(contents(v) == (std::vector<int>{1, 2, 3}));

    v.pop_back();
    v.pop_back();
    v.pop_back();
    CHECK(v.empty());
}

template<typename T>
static void elementAccess(char const* name) {
    Kvasir::Test::test(name);

    StaticVector<T, 4> v;
    v.push_back(make<T>(10));
    v.push_back(make<T>(20));
    v.push_back(make<T>(30));

    CHECK(v[0] == make<T>(10));
    CHECK(v[2] == make<T>(30));
    CHECK(v.front() == make<T>(10));
    CHECK(v.back() == make<T>(30));
    CHECK(v.data() == &v[0]);

    auto const& cv = v;
    CHECK(cv[1] == make<T>(20));
    CHECK(cv.front() == make<T>(10));
    CHECK(cv.back() == make<T>(30));
    CHECK(cv.data() == &cv[0]);

    // writing through operator[] and front/back
    v[1] = make<T>(99);
    CHECK(v[1] == make<T>(99));
    v.front() = make<T>(11);
    v.back()  = make<T>(33);
    CHECK(contents(v) == (std::vector<int>{11, 99, 33}));
}

template<typename T>
static void iterators(char const* name) {
    Kvasir::Test::test(name);

    StaticVector<T, 5> v;
    for(int i = 0; i != 5; ++i) { v.push_back(make<T>(i)); }

    CHECK(std::distance(v.begin(), v.end()) == 5);
    CHECK(contents(v) == (std::vector<int>{0, 1, 2, 3, 4}));

    // reverse iteration
    std::vector<int> reversed;
    for(auto it = v.rbegin(); it != v.rend(); ++it) {
        if constexpr(std::is_same_v<T, Tracked>) {
            reversed.push_back(it->value);
        } else {
            reversed.push_back(static_cast<int>(*it));
        }
    }
    CHECK(reversed == (std::vector<int>{4, 3, 2, 1, 0}));

    auto const& cv = v;
    CHECK(std::distance(cv.begin(), cv.end()) == 5);
    CHECK(std::distance(cv.rbegin(), cv.rend()) == 5);
    CHECK(std::distance(v.cbegin(), v.cend()) == 5);
}

template<typename T>
static void insertSingle(char const* name) {
    Kvasir::Test::test(name);

    StaticVector<T, 8> v;
    v.push_back(make<T>(1));
    v.push_back(make<T>(2));
    v.push_back(make<T>(3));

    // insert in the middle
    auto it = v.insert(v.begin() + 1, make<T>(99));
    CHECK(contents(v) == (std::vector<int>{1, 99, 2, 3}));
    CHECK(it == v.begin() + 1);

    // insert at the front
    v.insert(v.begin(), make<T>(0));
    CHECK(contents(v) == (std::vector<int>{0, 1, 99, 2, 3}));

    // insert at the end
    v.insert(v.end(), make<T>(4));
    CHECK(contents(v) == (std::vector<int>{0, 1, 99, 2, 3, 4}));
}

template<typename T>
static void insertCountAndRange(char const* name) {
    Kvasir::Test::test(name);

    {
        StaticVector<T, 8> v;
        v.push_back(make<T>(1));
        v.push_back(make<T>(2));

        auto it = v.insert(v.begin() + 1, size_t{3}, make<T>(7));
        CHECK(contents(v) == (std::vector<int>{1, 7, 7, 7, 2}));
        CHECK(it == v.begin() + 1);

        // inserting zero elements is a no-op
        v.insert(v.begin(), size_t{0}, make<T>(5));
        CHECK(contents(v) == (std::vector<int>{1, 7, 7, 7, 2}));
    }

    {
        StaticVector<T, 8> v;
        v.push_back(make<T>(1));
        v.push_back(make<T>(4));

        std::array<T, 2> const source{make<T>(2), make<T>(3)};
        auto                   it = v.insert(v.begin() + 1, source.begin(), source.end());
        CHECK(contents(v) == (std::vector<int>{1, 2, 3, 4}));
        CHECK(it == v.begin() + 1);

        // empty range
        v.insert(v.begin(), source.begin(), source.begin());
        CHECK(contents(v) == (std::vector<int>{1, 2, 3, 4}));
    }
}

template<typename T>
static void emplaceAndEmplaceBack(char const* name) {
    Kvasir::Test::test(name);

    StaticVector<T, 8> v;
    v.emplace_back(1);
    v.emplace_back(3);
    CHECK(contents(v) == (std::vector<int>{1, 3}));

    auto it = v.emplace(v.begin() + 1, 2);
    CHECK(contents(v) == (std::vector<int>{1, 2, 3}));
    CHECK(it == v.begin() + 1);

    v.emplace(v.end(), 4);
    CHECK(contents(v) == (std::vector<int>{1, 2, 3, 4}));

    v.emplace(v.begin(), 0);
    CHECK(contents(v) == (std::vector<int>{0, 1, 2, 3, 4}));
}

template<typename T>
static void eraseSingleAndRange(char const* name) {
    Kvasir::Test::test(name);

    StaticVector<T, 8> v;
    for(int i = 0; i != 6; ++i) { v.push_back(make<T>(i)); }

    auto it = v.erase(v.begin() + 2);
    CHECK(contents(v) == (std::vector<int>{0, 1, 3, 4, 5}));
    CHECK(it == v.begin() + 2);

    it = v.erase(v.begin());
    CHECK(contents(v) == (std::vector<int>{1, 3, 4, 5}));
    CHECK(it == v.begin());

    it = v.erase(v.end() - 1);
    CHECK(contents(v) == (std::vector<int>{1, 3, 4}));
    CHECK(it == v.end());

    // range erase from the middle
    it = v.erase(v.begin(), v.begin() + 2);
    CHECK(contents(v) == (std::vector<int>{4}));
    CHECK(it == v.begin());

    // erasing an empty range is a no-op
    it = v.erase(v.begin(), v.begin());
    CHECK(contents(v) == (std::vector<int>{4}));

    // erasing everything
    v.erase(v.begin(), v.end());
    CHECK(v.empty());
}

template<typename T>
static void clearAndResize(char const* name) {
    Kvasir::Test::test(name);

    StaticVector<T, 8> v;
    for(int i = 0; i != 5; ++i) { v.push_back(make<T>(i)); }

    v.resize(3);
    CHECK(contents(v) == (std::vector<int>{0, 1, 2}));

    // growing with the default constructed value
    v.resize(5);
    CHECK_EQ(v.size(), 5U);
    CHECK(contents(v) == (std::vector<int>{0, 1, 2, 0, 0}));

    // growing with an explicit value
    v.resize(7, make<T>(9));
    CHECK(contents(v) == (std::vector<int>{0, 1, 2, 0, 0, 9, 9}));

    // resizing to the current size does nothing
    v.resize(7);
    CHECK_EQ(v.size(), 7U);

    v.resize(0);
    CHECK(v.empty());

    v.push_back(make<T>(1));
    v.clear();
    CHECK(v.empty());
    CHECK_EQ(v.size(), 0U);
}

template<typename T>
static void copyAndMove(char const* name) {
    Kvasir::Test::test(name);

    StaticVector<T, 8> a;
    for(int i = 0; i != 4; ++i) { a.push_back(make<T>(i)); }

    // copy construction
    StaticVector<T, 8> b{a};
    CHECK(contents(b) == (std::vector<int>{0, 1, 2, 3}));
    CHECK(contents(a) == (std::vector<int>{0, 1, 2, 3}));   // source untouched

    // copy assignment over a non-empty target
    StaticVector<T, 8> c;
    c.push_back(make<T>(42));
    c.push_back(make<T>(43));
    c.push_back(make<T>(44));
    c = a;
    CHECK(contents(c) == (std::vector<int>{0, 1, 2, 3}));

    // move construction
    StaticVector<T, 8> d{std::move(b)};
    CHECK(contents(d) == (std::vector<int>{0, 1, 2, 3}));

    // move assignment over a non-empty target
    StaticVector<T, 8> e;
    e.push_back(make<T>(7));
    e = std::move(c);
    CHECK(contents(e) == (std::vector<int>{0, 1, 2, 3}));

    // self assignment must not corrupt the vector; the reference keeps the compiler from
    // diagnosing it as an obviously redundant self-assign
    auto& selfRef = a;
    a             = selfRef;
    CHECK_EQ(a.size(), 4U);
    CHECK(contents(a) == (std::vector<int>{0, 1, 2, 3}));
}

template<typename T>
static void swapping(char const* name) {
    Kvasir::Test::test(name);

    StaticVector<T, 8> a;
    a.push_back(make<T>(1));
    a.push_back(make<T>(2));

    StaticVector<T, 8> b;
    b.push_back(make<T>(7));
    b.push_back(make<T>(8));
    b.push_back(make<T>(9));

    a.swap(b);
    CHECK(contents(a) == (std::vector<int>{7, 8, 9}));
    CHECK(contents(b) == (std::vector<int>{1, 2}));

    // swapping with an empty vector
    StaticVector<T, 8> empty;
    a.swap(empty);
    CHECK(a.empty());
    CHECK(contents(empty) == (std::vector<int>{7, 8, 9}));
}

template<typename T>
static void constructorsAndAssign(char const* name) {
    Kvasir::Test::test(name);

    // n default constructed elements
    {
        StaticVector<T, 8> v(3u);
        CHECK_EQ(v.size(), 3U);
        CHECK(contents(v) == (std::vector<int>{0, 0, 0}));
    }

    // n copies of a value
    {
        StaticVector<T, 8> v(3u, make<T>(5));
        CHECK(contents(v) == (std::vector<int>{5, 5, 5}));
    }

    // from an iterator range
    {
        std::array<T, 3> const source{make<T>(1), make<T>(2), make<T>(3)};
        StaticVector<T, 8>     v(source.begin(), source.end());
        CHECK(contents(v) == (std::vector<int>{1, 2, 3}));
    }

    // assign from a range, over an existing payload
    {
        StaticVector<T, 8>     v(5u, make<T>(9));
        std::array<T, 2> const source{make<T>(1), make<T>(2)};
        v.assign(source.begin(), source.end());
        CHECK(contents(v) == (std::vector<int>{1, 2}));
    }

    // assign n copies
    {
        StaticVector<T, 8> v(2u, make<T>(9));
        v.assign(size_t{4}, make<T>(3));
        CHECK(contents(v) == (std::vector<int>{3, 3, 3, 3}));
    }

    // assign an empty range clears the vector
    {
        StaticVector<T, 8>     v(3u, make<T>(9));
        std::array<T, 1> const source{make<T>(1)};
        v.assign(source.begin(), source.begin());
        CHECK(v.empty());
    }
}

template<typename T>
static void comparisons(char const* name) {
    Kvasir::Test::test(name);

    StaticVector<T, 8> a;
    a.push_back(make<T>(1));
    a.push_back(make<T>(2));

    StaticVector<T, 8> b;
    b.push_back(make<T>(1));
    b.push_back(make<T>(2));

    StaticVector<T, 8> c;
    c.push_back(make<T>(1));
    c.push_back(make<T>(3));

    StaticVector<T, 8> shorter;
    shorter.push_back(make<T>(1));

    StaticVector<T, 8> empty;

    CHECK(a == b);
    CHECK(!(a != b));
    CHECK(a != c);
    CHECK(a != shorter);

    // ordering is lexicographic: the first differing element decides, and on a common
    // prefix the shorter vector compares less
    CHECK(a < c);
    CHECK(!(c < a));
    CHECK(shorter < a);
    CHECK(!(a < shorter));
    CHECK(empty < a);
    CHECK(!(a < empty));

    // a strict weak ordering is irreflexive
    CHECK(!(a < b));
    CHECK(!(empty < empty));

    CHECK(a <= b);
    CHECK(a >= b);
    CHECK(c > a);
    CHECK(c >= a);
    CHECK(!(a > c));
    CHECK(a <= c);
    CHECK(!(c <= a));
    CHECK(shorter <= a);
    CHECK(a >= shorter);
    CHECK(empty <= empty);
    CHECK(empty >= empty);

    // a larger leading element wins regardless of length
    StaticVector<T, 8> big;
    big.push_back(make<T>(9));
    CHECK(a < big);
    CHECK(big > a);
    CHECK(!(big < a));
}

// ---------------------------------------------------------------------------
// lifetime accounting: after every scenario every constructed element must have been
// destroyed exactly once
static void noLeaks(char const* name,
                    void (*scenario)(char const*)) {
    Tracked::reset();
    scenario(name);
    if(Tracked::alive() != 0) {
        ++Kvasir::Test::failures;
        std::print("FAIL [{}] {} live Tracked objects leaked (constructed {}, destroyed {})\n",
                   name,
                   Tracked::alive(),
                   Tracked::constructed,
                   Tracked::destroyed);
    }
}

// destroying a vector must destroy exactly the elements it holds, no more and no fewer
static void destructorDestroysElements() {
    Kvasir::Test::test("destructorDestroysElements");
    Tracked::reset();

    {
        StaticVector<Tracked, 8> v;
        v.emplace_back(1);
        v.emplace_back(2);
        v.emplace_back(3);
        CHECK_EQ(Tracked::alive(), 3);
    }
    CHECK_EQ(Tracked::alive(), 0);

    // clear destroys the elements immediately, not at scope exit
    Tracked::reset();
    {
        StaticVector<Tracked, 8> v;
        v.emplace_back(1);
        v.emplace_back(2);
        CHECK_EQ(Tracked::alive(), 2);
        v.clear();
        CHECK_EQ(Tracked::alive(), 0);
    }
    CHECK_EQ(Tracked::alive(), 0);

    // pop_back destroys one element
    Tracked::reset();
    {
        StaticVector<Tracked, 8> v;
        v.emplace_back(1);
        v.emplace_back(2);
        v.pop_back();
        CHECK_EQ(Tracked::alive(), 1);
    }
    CHECK_EQ(Tracked::alive(), 0);

    // erase destroys exactly the erased range
    Tracked::reset();
    {
        StaticVector<Tracked, 8> v;
        for(int i = 0; i != 6; ++i) { v.emplace_back(i); }
        CHECK_EQ(Tracked::alive(), 6);
        v.erase(v.begin() + 1, v.begin() + 4);
        CHECK_EQ(Tracked::alive(), 3);
        CHECK(contents(v) == (std::vector<int>{0, 4, 5}));
    }
    CHECK_EQ(Tracked::alive(), 0);

    // shrinking resize destroys the removed tail
    Tracked::reset();
    {
        StaticVector<Tracked, 8> v;
        for(int i = 0; i != 5; ++i) { v.emplace_back(i); }
        v.resize(2);
        CHECK_EQ(Tracked::alive(), 2);
    }
    CHECK_EQ(Tracked::alive(), 0);
}

// ---------------------------------------------------------------------------
// the zero_sized storage backend
static void zeroCapacity() {
    Kvasir::Test::test("zeroCapacity");

    StaticVector<int, 0> v;
    CHECK(v.empty());
    CHECK(v.full());   // a zero capacity vector is always both empty and full
    CHECK_EQ(v.size(), 0U);
    CHECK_EQ(v.capacity(), 0U);
    CHECK(v.begin() == v.end());
    CHECK(v.data() == nullptr);

    StaticVector<int, 0> other;
    CHECK(v == other);
    CHECK(!(v < other));

    // copy and move of an always empty vector
    StaticVector<int, 0> copy{v};
    CHECK(copy.empty());
    StaticVector<int, 0> moved{std::move(other)};
    CHECK(moved.empty());
}

// ---------------------------------------------------------------------------
// capacity boundary: filling exactly to capacity must work in every mode
template<typename T>
static void fillToCapacity(char const* name) {
    Kvasir::Test::test(name);

    StaticVector<T, 3> v;
    v.push_back(make<T>(1));
    v.push_back(make<T>(2));
    v.push_back(make<T>(3));
    CHECK(v.full());
    CHECK_EQ(v.size(), v.capacity());

    // inserting into a full vector is only legal after making room
    v.pop_back();
    v.insert(v.begin(), make<T>(0));
    CHECK(contents(v) == (std::vector<int>{0, 1, 2}));
    CHECK(v.full());

    // a capacity 1 vector
    StaticVector<T, 1> one;
    CHECK(one.empty());
    one.push_back(make<T>(5));
    CHECK(one.full());
    CHECK(one.front() == one.back());
    one.pop_back();
    CHECK(one.empty());
}

// initializer_list construction
static void initializerList() {
    Kvasir::Test::test("initializerList");

    StaticVector<int, 8> v{1, 2, 3};
    CHECK(contents(v) == (std::vector<int>{1, 2, 3}));
    CHECK_EQ(v.size(), 3U);

    StaticVector<int, 8> empty{};
    CHECK(empty.empty());

    // exactly at capacity
    StaticVector<int, 3> exact{7, 8, 9};
    CHECK(exact.full());
    CHECK(contents(exact) == (std::vector<int>{7, 8, 9}));

    // assign from an initializer_list over an existing payload
    v.assign({4, 5});
    CHECK(contents(v) == (std::vector<int>{4, 5}));

    // insert an initializer_list
    v.insert(v.begin() + 1, {9, 9});
    CHECK(contents(v) == (std::vector<int>{4, 9, 9, 5}));
}

// the vector must work with the standard algorithms
static void worksWithAlgorithms() {
    Kvasir::Test::test("worksWithAlgorithms");

    StaticVector<int, 8> v{5, 3, 1, 4, 2};

    std::sort(v.begin(), v.end());
    CHECK(contents(v) == (std::vector<int>{1, 2, 3, 4, 5}));

    CHECK(std::accumulate(v.begin(), v.end(), 0) == 15);
    CHECK(std::find(v.begin(), v.end(), 3) == v.begin() + 2);
    CHECK(std::is_sorted(v.begin(), v.end()));

    std::reverse(v.begin(), v.end());
    CHECK(contents(v) == (std::vector<int>{5, 4, 3, 2, 1}));
}

int main() {
    // trivial storage backend
    emptyState<int>("emptyState<int>");
    pushPopBack<int>("pushPopBack<int>");
    elementAccess<int>("elementAccess<int>");
    iterators<int>("iterators<int>");
    insertSingle<int>("insertSingle<int>");
    insertCountAndRange<int>("insertCountAndRange<int>");
    emplaceAndEmplaceBack<int>("emplaceAndEmplaceBack<int>");
    eraseSingleAndRange<int>("eraseSingleAndRange<int>");
    clearAndResize<int>("clearAndResize<int>");
    copyAndMove<int>("copyAndMove<int>");
    swapping<int>("swapping<int>");
    constructorsAndAssign<int>("constructorsAndAssign<int>");
    comparisons<int>("comparisons<int>");
    fillToCapacity<int>("fillToCapacity<int>");

    // non_trivial storage backend, each scenario additionally checked for leaks
    noLeaks("emptyState<Tracked>", &emptyState<Tracked>);
    noLeaks("pushPopBack<Tracked>", &pushPopBack<Tracked>);
    noLeaks("elementAccess<Tracked>", &elementAccess<Tracked>);
    noLeaks("iterators<Tracked>", &iterators<Tracked>);
    noLeaks("insertSingle<Tracked>", &insertSingle<Tracked>);
    noLeaks("insertCountAndRange<Tracked>", &insertCountAndRange<Tracked>);
    noLeaks("emplaceAndEmplaceBack<Tracked>", &emplaceAndEmplaceBack<Tracked>);
    noLeaks("eraseSingleAndRange<Tracked>", &eraseSingleAndRange<Tracked>);
    noLeaks("clearAndResize<Tracked>", &clearAndResize<Tracked>);
    noLeaks("copyAndMove<Tracked>", &copyAndMove<Tracked>);
    noLeaks("swapping<Tracked>", &swapping<Tracked>);
    noLeaks("constructorsAndAssign<Tracked>", &constructorsAndAssign<Tracked>);
    noLeaks("comparisons<Tracked>", &comparisons<Tracked>);
    noLeaks("fillToCapacity<Tracked>", &fillToCapacity<Tracked>);

    destructorDestroysElements();
    zeroCapacity();
    initializerList();
    worksWithAlgorithms();

    return Kvasir::Test::report();
}
