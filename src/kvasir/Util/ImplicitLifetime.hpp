#pragma once
// Kvasir::ImplicitLifetime<T>: T is an implicit-lifetime type ([basic.types.general]) - a scalar, an array, or a class
// with a trivial eligible constructor and a trivial, non-deleted destructor (or an aggregate whose destructor is not
// user-provided). What starts the lifetime of storage (a byte array, memcpy) creates such a T in it as well.
//
//     static_assert(Kvasir::ImplicitLifetime<Request>);   // a byte array may hold Requests
//
// std::is_implicit_lifetime_v where the library has it, else the compiler's builtin (clang). arm GCC 15 with its
// libstdc++ has neither: there it is the part of the definition the C++20 traits can see - scalars, arrays, classes
// with a trivial default, copy or move constructor and a trivial destructor. That part is never wrong towards "yes"
// (an aggregate with a non-trivial constructor reads "no"), so storage chosen by it is never raw for a T that needs
// a constructor run; the checks below compare it with the real trait wherever one exists.
#include <cstddef>
#include <type_traits>

namespace Kvasir {
namespace Detail {
    /// The part of the definition the C++20 type traits can see.
    template<typename T>
    inline constexpr bool implicitLifetimeByTraits
      = std::is_scalar_v<T> || std::is_array_v<T>
     || ((std::is_class_v<T> || std::is_union_v<T>) && std::is_trivially_destructible_v<T>
         && (std::is_trivially_default_constructible_v<T>
             || std::is_trivially_copy_constructible_v<T>
             || std::is_trivially_move_constructible_v<T>));
}   // namespace Detail

template<typename T>
concept ImplicitLifetime =
#ifdef __cpp_lib_is_implicit_lifetime
  std::is_implicit_lifetime_v<T>;
#elif __has_builtin(__builtin_is_implicit_lifetime)
  __builtin_is_implicit_lifetime(T);
#else
  Detail::implicitLifetimeByTraits<std::remove_cv_t<T>>;
#endif

namespace Detail { namespace ImplicitLifetimeChecks {
    struct Plain {
        int   a;
        float b;
    };

    // a non-trivial default constructor, a trivial copy: still implicit-lifetime
    struct WithDefaults {
        int timeout = 100;
    };

    // declared, never defined: the traits ask in unevaluated operands only
    struct OwnDestructor {   // a user-provided destructor: never
        ~OwnDestructor();
    };

    struct OnlyBuilt {   // no trivial constructor and no aggregate: never
        OnlyBuilt();
        OnlyBuilt(OnlyBuilt const&);
    };

    static_assert(ImplicitLifetime<int> && ImplicitLifetime<std::byte>
                    && ImplicitLifetime<int const> && ImplicitLifetime<OnlyBuilt[2]>
                    && ImplicitLifetime<Plain> && ImplicitLifetime<WithDefaults>,
                  "scalars, arrays and classes with a trivial constructor and destructor");
    static_assert(!ImplicitLifetime<OwnDestructor> && !ImplicitLifetime<OnlyBuilt>,
                  "a non-trivial destructor, or no trivial constructor outside an aggregate");

    // the traits' part never says yes where the real trait says no (trivially so where it is the only one)
    template<typename... Ts>
    inline constexpr bool byTraitsWithin
      = ((!implicitLifetimeByTraits<Ts> || ImplicitLifetime<Ts>) && ...);
    static_assert(byTraitsWithin<int,
                                 std::byte,
                                 OnlyBuilt[2],
                                 Plain,
                                 WithDefaults,
                                 OwnDestructor,
                                 OnlyBuilt>,
                  "implicitLifetimeByTraits is a part of the definition");
}}   // namespace Detail::ImplicitLifetimeChecks
}   // namespace Kvasir
