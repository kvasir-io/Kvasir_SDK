// Kvasir::ImplicitLifetime and the fallback a standard library without std::is_implicit_lifetime gets
// (Detail::implicitLifetimeByTraits): on a host with the real trait, the fallback is compared with it type by type -
// it may say "no" where the trait says "yes" (the aggregate route), never the other way round.
#include "kvasir/Atomic/Queue.hpp"
#include "kvasir/Util/ImplicitLifetime.hpp"
#include "test_harness.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace {
using Kvasir::ImplicitLifetime;
using Kvasir::Detail::implicitLifetimeByTraits;

enum class Kind : std::uint8_t { a, b };

struct Plain {
    int   a;
    float b;
};

struct WithDefaults {   // non-trivial default constructor, trivial copy
    std::uint32_t timeout = 100;
    Kind          kind    = Kind::b;
};

struct NoDefault {   // no default constructor at all, a trivial copy

    explicit NoDefault(int v) : value{v} {}

    int value;
};

// declared, never defined: the traits ask in unevaluated operands only
struct OwnCopy {   // user-provided default and copy constructors, trivial destructor
    OwnCopy();
    OwnCopy(OwnCopy const&);
};

struct
  HoldsOwnCopy {   // an aggregate whose destructor is not user-provided: implicit-lifetime, but no trivial constructor
    OwnCopy member;
};

struct OwnDestructor {
    ~OwnDestructor();
};

struct
  DeletedDestructor {   // an aggregate: a deleted destructor is not user-provided, so implicit-lifetime
    ~DeletedDestructor() = delete;
};

struct
  DeletedDestructorNotAggregate {   // a user-declared constructor, no trivial destructor: never
    explicit DeletedDestructorNotAggregate(int);
    ~DeletedDestructorNotAggregate() = delete;
};

struct Virtual {   // a vtable: no trivial constructor, not an aggregate
    virtual ~Virtual() = default;
    virtual void f();
};

union Either {
    std::uint32_t word;
    float         number;
};

union HoldsOwnDestructor {   // the union's destructor is deleted
    OwnDestructor member;
};

using Closure = decltype([] {});

// "no" from the fallback where the real trait says "yes" is allowed; "yes" where it says "no" is not
template<typename T>
inline constexpr bool fallbackIsSafe = !implicitLifetimeByTraits<T> || ImplicitLifetime<T>;

// scalars, cv-qualified ones, arrays (of anything)
static_assert(implicitLifetimeByTraits<int> && implicitLifetimeByTraits<std::byte>
              && implicitLifetimeByTraits<Kind> && implicitLifetimeByTraits<int*>
              && implicitLifetimeByTraits<int Plain::*>
              && implicitLifetimeByTraits<std::nullptr_t>);
static_assert(ImplicitLifetime<int const> && ImplicitLifetime<int volatile>
              && ImplicitLifetime<Plain const volatile>);
static_assert(implicitLifetimeByTraits<OwnDestructor[2]> && implicitLifetimeByTraits<Virtual[]>);
// classes and unions with a trivial constructor and destructor
static_assert(implicitLifetimeByTraits<Plain> && implicitLifetimeByTraits<WithDefaults>
              && implicitLifetimeByTraits<NoDefault> && implicitLifetimeByTraits<Either>
              && implicitLifetimeByTraits<Closure>
              && implicitLifetimeByTraits<std::array<WithDefaults,
                                                     3>>);
// never
static_assert(!implicitLifetimeByTraits<OwnDestructor>
              && !implicitLifetimeByTraits<DeletedDestructor> && !implicitLifetimeByTraits<Virtual>
              && !implicitLifetimeByTraits<HoldsOwnDestructor> && !implicitLifetimeByTraits<OwnCopy>
              && !implicitLifetimeByTraits<void> && !implicitLifetimeByTraits<int&>
              && !implicitLifetimeByTraits<void()>);
static_assert(!ImplicitLifetime<OwnDestructor> && !ImplicitLifetime<DeletedDestructorNotAggregate>
              && !ImplicitLifetime<Virtual> && !ImplicitLifetime<OwnCopy> && !ImplicitLifetime<void>
              && !ImplicitLifetime<int&>);
// the gap: aggregates without a trivial constructor or destructor
static_assert(!implicitLifetimeByTraits<HoldsOwnCopy>
              && !implicitLifetimeByTraits<DeletedDestructor>
              && !implicitLifetimeByTraits<DeletedDestructorNotAggregate>);

static_assert(fallbackIsSafe<int> && fallbackIsSafe<Plain> && fallbackIsSafe<WithDefaults>
              && fallbackIsSafe<NoDefault> && fallbackIsSafe<OwnCopy>
              && fallbackIsSafe<HoldsOwnCopy> && fallbackIsSafe<OwnDestructor>
              && fallbackIsSafe<DeletedDestructor> && fallbackIsSafe<DeletedDestructorNotAggregate>
              && fallbackIsSafe<Virtual> && fallbackIsSafe<Either>
              && fallbackIsSafe<HoldsOwnDestructor> && fallbackIsSafe<Closure>
              && fallbackIsSafe<OwnDestructor[2]>);

#ifdef __cpp_lib_is_implicit_lifetime
// with the library's trait: the concept is it, and the gap is a real "yes"
static_assert(ImplicitLifetime<HoldsOwnCopy> == std::is_implicit_lifetime_v<HoldsOwnCopy>);
static_assert(std::is_implicit_lifetime_v<HoldsOwnCopy>
              && std::is_implicit_lifetime_v<DeletedDestructor>);
#endif

// what the queue takes from it: a request with default member values gets raw (zero-filled) slots on every toolchain
static_assert(std::is_same_v<Kvasir::Atomic::Detail::Slots<WithDefaults,
                                                           4>,
                             Kvasir::Atomic::Detail::RawSlots<WithDefaults,
                                                              4>>);
static_assert(std::is_same_v<Kvasir::Atomic::Detail::Slots<NoDefault,
                                                           4>,
                             Kvasir::Atomic::Detail::RawSlots<NoDefault,
                                                              4>>);

void decidedAtCompileTime() {
    Kvasir::Test::test("decidedAtCompileTime");
    CHECK(ImplicitLifetime<WithDefaults>);
    CHECK(!ImplicitLifetime<OwnDestructor>);
    CHECK(fallbackIsSafe<HoldsOwnCopy>);
}
}   // namespace

int main() {
    decidedAtCompileTime();
    return Kvasir::Test::report();
}
