#pragma once
// The one idiom for "the config's member if it has one, else a default".
//
// Which idiom for a knob:
//  - a value knob of a component's Config struct:   KVASIR_CONFIG_OR(Config, member, fallback)
//  - a type knob:  template<class C> using FooOf = typename C::Foo;
//                  using Foo = ::Kvasir::Config::TypeOr<Config, DefaultFoo, FooOf>;
//    (always fully qualified: inside a template whose parameter is called Config, `Config::` is the parameter)
//  - scalars that must be structural (template arguments): an NTTP aggregate, integers suffixed ...Ms / ...Hz
//    (RateLimiterConfig)
//  - durations: probe the chrono value, convert at the use (std::chrono::duration_cast; a library may have a helper
//    that refuses a bare integer)
//  - derive-from-XDefaults (CellStore, Retry, driver defaults): inside the derived struct the defaults' names win
//    over the firmware's globals - write ::Crc, not Crc
//  - members: value members lowerCamel, everywhere; type members PascalCase
//  - renaming a member renames every config that sets it in the same change; no guard keeps the name it had
//
// Only inside a template where Cfg is dependent: outside one, `requires { Cfg::member; }` is a hard error, not false.
// Cfg must be one token or a parenthesis-free name (an alias for a template-id with commas); the fallback may contain
// commas. Evaluated at compile time (consteval): a member that is not a constant expression is a compile error.

#define KVASIR_CONFIG_OR(Cfg, member, ...)        \
    ([]() consteval {                             \
        if constexpr(requires { Cfg::member; }) { \
            return Cfg::member;                   \
        } else {                                  \
            return __VA_ARGS__;                   \
        }                                         \
    }())

namespace Kvasir::Config {
template<typename C, typename Default, template<typename> typename Member>
struct TypeOrT {
    using type = Default;
};

template<typename C, typename Default, template<typename> typename Member>
    requires requires { typename Member<C>; }
struct TypeOrT<C, Default, Member> {
    using type = Member<C>;
};

/// Member<C> if it names a type, else Default.
template<typename C, typename Default, template<typename> typename Member>
using TypeOr = typename TypeOrT<C, Default, Member>::type;
}   // namespace Kvasir::Config
