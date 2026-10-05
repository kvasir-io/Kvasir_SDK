#pragma once
// Runtime checks that log their operands, then panic (Kvasir::Panic::Cause::checkFailed):
//
//     KVASIR_CHECK_LE(data.size(), free(), "fifo {} overflow", id_);
//     -> [crit] fifo.cpp:41 CHECK(data.size() <= free()) failed: 96 vs 64: fifo 2 overflow
//     KVASIR_CHECK_NE(mode, Mode::invalid);            // an enum is logged by name
//     KVASIR_CHECK(ready(), "state {}", state_);       // unary: no values of its own
//     KVASIR_DCHECK_LT(i, size());                     // a debug check: off in release_log and release
//     if(!KVASIR_SOFT_CHECK(rx.size() <= Depth, "rx overrun, {} bytes", rx.size())) { rx.clear(); }
//     KVASIR_CHECK_UNREACHABLE("state {}", state_);
//
// The expression's text is part of the cataloged format string: only the values travel. Integers compare by
// value (std::cmp_less: -1 != 0xFFFFFFFFu). Each operand is evaluated exactly once; at level 0 none is (but each is
// still type-checked). The failure path is cold and out of line. In a constant evaluation a failing check is a
// compile error naming checkFailedInConstantExpression.
//
// Levels, per target from CMake (kvasir_executable_variants: CHECK_LEVEL, CHECK_LEVEL_RELEASE, DCHECK_LEVEL,
// DCHECK_LEVEL_RELEASE): KVASIR_CHECK_LEVEL / KVASIR_DCHECK_LEVEL 0 off, 1 bare (panic, no log line: the site is
// the panic record's pc), 2 full. Every TU of an image must see the same level - never define them in a header.
//
// A failed KVASIR_SOFT_CHECK logs an error and returns false; what follows is the injected policy:
//     template<> inline constexpr auto Kvasir::Check::injectedSoftCheckPolicy<> = Kvasir::Check::Escalate{};
// The default escalates to a panic where KVASIR_SOFT_CHECK_ESCALATE is 1 (the sanitize variant) and only logs
// elsewhere.
//
// Not in a RAM function that runs with XIP off: the failure path is in flash.
#include "kvasir/Util/Panic.hpp"

#ifdef USE_UC_LOG
    #include "uc_log/uc_log.hpp"
#endif

#include <concepts>
#include <cstdint>
#include <type_traits>
#include <utility>

#ifndef KVASIR_CHECK_LEVEL
    #define KVASIR_CHECK_LEVEL 2
#endif
#ifndef KVASIR_DCHECK_LEVEL
    #define KVASIR_DCHECK_LEVEL KVASIR_CHECK_LEVEL
#endif
#ifndef KVASIR_SOFT_CHECK_ESCALATE
    #define KVASIR_SOFT_CHECK_ESCALATE 0
#endif

namespace Kvasir::Check {
namespace Detail {
    // std::cmp_* for two standard integers (bool and character types excluded), the type's operator otherwise.
    template<typename T>
    concept StdInteger = std::integral<T> && !std::same_as<T, bool> && !std::same_as<T, char>
                      && !std::same_as<T, char8_t> && !std::same_as<T, char16_t>
                      && !std::same_as<T, char32_t> && !std::same_as<T, wchar_t>;

#define KVASIR_DETAIL_CHECK_COMPARE(Name, op, cmp)                   \
    struct Name {                                                    \
        template<typename A,                                         \
                 typename B>                                         \
        [[nodiscard]] static constexpr bool operator()(A const& a,   \
                                                       B const& b) { \
            if constexpr(StdInteger<A> && StdInteger<B>) {           \
                return std::cmp(a, b);                               \
            } else {                                                 \
                return a op b;                                       \
            }                                                        \
        }                                                            \
    };
    KVASIR_DETAIL_CHECK_COMPARE(Eq,
                                ==,
                                cmp_equal)
    KVASIR_DETAIL_CHECK_COMPARE(Ne,
                                !=,
                                cmp_not_equal)
    KVASIR_DETAIL_CHECK_COMPARE(Lt,
                                <,
                                cmp_less)
    KVASIR_DETAIL_CHECK_COMPARE(Le,
                                <=,
                                cmp_less_equal)
    KVASIR_DETAIL_CHECK_COMPARE(Gt,
                                >,
                                cmp_greater)
    KVASIR_DETAIL_CHECK_COMPARE(Ge,
                                >=,
                                cmp_greater_equal)
#undef KVASIR_DETAIL_CHECK_COMPARE

    // An operand, evaluated once. Small and trivially copyable: by value - a bit-field binds, a volatile is read
    // once, nothing has its address taken (no stack protector, no spill). Anything else: a reference member of
    // an aggregate, so a prvalue's lifetime is extended to the end of the check.
    template<typename T,
             bool = std::is_trivially_copyable_v<std::remove_cvref_t<T>>
                 && (sizeof(std::remove_cvref_t<T>) <= 8)>
    struct Hold {
        std::remove_cvref_t<T> value;
    };

    template<typename T>
    struct Hold<T, false> {
        std::remove_reference_t<T> const& value;
    };

    // How the cold path takes an operand: as uc_log takes a log argument (uc_log.hpp LogArgument_t) - by
    // value when trivially copyable, else by reference.
    template<typename T>
    using Arg = std::conditional_t<std::is_array_v<std::remove_cvref_t<T>>
                                     || !std::is_trivially_copyable_v<std::remove_cvref_t<T>>,
                                   std::remove_cvref_t<T> const&,
                                   std::remove_cvref_t<T>>;

    // Named in the dead arm of a constant-false conditional: nothing evaluated, everything type-checked
    // (uc_log's no-log form, without uc_log).
    template<typename... Ts>
    constexpr int touch(Ts const&...) noexcept {
        return 0;
    }

    // Reached only by a check that fails inside a constant evaluation: not constexpr, so the compiler stops
    // here and names it.
    inline void checkFailedInConstantExpression() {}

    // The site in the panic record is failed()'s caller - the check's cold path, inside the function that
    // holds the check - not failed() itself.
    [[noreturn,
      gnu::cold,
      gnu::noinline]] inline void
    failed() {
        ::Kvasir::Panic::raiseAt(::Kvasir::Panic::Cause::checkFailed,
                                 static_cast<std::uint32_t>(
                                   reinterpret_cast<std::uintptr_t>(__builtin_return_address(0))));
    }
}   // namespace Detail

// What a failed KVASIR_SOFT_CHECK does after its log line.
struct Continue {
    static constexpr void operator()() {}
};

struct Escalate {
    [[noreturn]] static void operator()() { Detail::failed(); }
};

template<typename...>
inline constexpr auto injectedSoftCheckPolicy = [] {
    if constexpr(KVASIR_SOFT_CHECK_ESCALATE != 0) {
        return Escalate{};
    } else {
        return Continue{};
    }
}();

namespace Detail {
    template<typename... Dummy,
             typename Log>
        requires(sizeof...(Dummy) == 0)
    [[gnu::cold,
      gnu::noinline]] bool
    softCheckFailed(Log const& log) {
        log();
        injectedSoftCheckPolicy<Dummy...>();
        return false;
    }
}   // namespace Detail
}   // namespace Kvasir::Check

#define KVASIR_DETAIL_CHECK_CAT2(a, b) a##b
#define KVASIR_DETAIL_CHECK_CAT(a, b)  KVASIR_DETAIL_CHECK_CAT2(a, b)

// A log line from a check: uc_log's when the image logs; otherwise never evaluated, the format not even formed.
#ifdef USE_UC_LOG
    #define KVASIR_DETAIL_CHECK_LOG(level, fmt, ...) \
        UC_LOG_IMPL(::uc_log::LogLevel::level,       \
                    __LINE__,                        \
                    __FILE_NAME__,                   \
                    fmt __VA_OPT__(, ) __VA_ARGS__)
#else
    #define KVASIR_DETAIL_CHECK_LOG(level, fmt, ...)                               \
        static_cast<void>(false ? ::Kvasir::Check::Detail::touch(__VA_ARGS__) : 0)
#endif

// The optional user part: (fmt, args...) -> " + ": " + fmt" and ", args...".
#define KVASIR_DETAIL_CHECK_FIRST(f, ...)     f
#define KVASIR_DETAIL_CHECK_REST(f, ...)      __VA_OPT__(, __VA_ARGS__)
#define KVASIR_DETAIL_CHECK_REST_BARE(f, ...) __VA_ARGS__
#define KVASIR_DETAIL_CHECK_USER_FMT(...)                                  \
    __VA_OPT__(+": "_sc + SC_LIFT(KVASIR_DETAIL_CHECK_FIRST(__VA_ARGS__)))
#define KVASIR_DETAIL_CHECK_USER_ARGS(...) __VA_OPT__(KVASIR_DETAIL_CHECK_REST(__VA_ARGS__))
#define KVASIR_DETAIL_CHECK_USER_ARGS_BARE(...)            \
    __VA_OPT__(KVASIR_DETAIL_CHECK_REST_BARE(__VA_ARGS__))

// The expression text with its braces escaped ({{, }}), so it is literal text in the format string.
#define KVASIR_DETAIL_CHECK_TEXT(text)                                                       \
    ::sc::escape(                                                                            \
      SC_LIFT(text),                                                                         \
      [](char kvasir_check_ch) { return kvasir_check_ch == '{' || kvasir_check_ch == '}'; }, \
      [](char kvasir_check_ch) { return kvasir_check_ch; })

#define KVASIR_DETAIL_CHECK_BIN_2(what, Cmp, opText, a, b, ...)                                   \
    do {                                                                                          \
        ::Kvasir::Check::Detail::Hold<decltype((a))> const kvasir_check_ha{(a)};                  \
        ::Kvasir::Check::Detail::Hold<decltype((b))> const kvasir_check_hb{(b)};                  \
        if(!::Kvasir::Check::Detail::Cmp{}(kvasir_check_ha.value, kvasir_check_hb.value))         \
          [[unlikely]]                                                                            \
        {                                                                                         \
            if consteval {                                                                        \
                ::Kvasir::Check::Detail::checkFailedInConstantExpression();                       \
            } else {                                                                              \
                [](::Kvasir::Check::Detail::Arg<decltype(kvasir_check_ha.value)> kvasir_check_va, \
                   ::Kvasir::Check::Detail::Arg<decltype(kvasir_check_hb.value)> kvasir_check_vb, \
                   auto const&... kvasir_check_extra) __attribute__((cold, noinline, noreturn)) { \
                    KVASIR_DETAIL_CHECK_LOG(                                                      \
                      crit,                                                                       \
                      KVASIR_DETAIL_CHECK_TEXT(what "(" #a " " opText " " #b ")")                 \
                        + " failed: {} vs {}"_sc KVASIR_DETAIL_CHECK_USER_FMT(__VA_ARGS__),       \
                      kvasir_check_va,                                                            \
                      kvasir_check_vb,                                                            \
                      kvasir_check_extra...);                                                     \
                    ::Kvasir::Check::Detail::failed();                                            \
                }(kvasir_check_ha.value,                                                          \
                  kvasir_check_hb.value KVASIR_DETAIL_CHECK_USER_ARGS(__VA_ARGS__));              \
            }                                                                                     \
        }                                                                                         \
    } while(false)

#define KVASIR_DETAIL_CHECK_BIN_1(what, Cmp, opText, a, b, ...)                           \
    do {                                                                                  \
        ::Kvasir::Check::Detail::Hold<decltype((a))> const kvasir_check_ha{(a)};          \
        ::Kvasir::Check::Detail::Hold<decltype((b))> const kvasir_check_hb{(b)};          \
        if(!::Kvasir::Check::Detail::Cmp{}(kvasir_check_ha.value, kvasir_check_hb.value)) \
          [[unlikely]]                                                                    \
        {                                                                                 \
            if consteval {                                                                \
                ::Kvasir::Check::Detail::checkFailedInConstantExpression();               \
            } else {                                                                      \
                ::Kvasir::Check::Detail::failed();                                        \
            }                                                                             \
        }                                                                                 \
    } while(false)

// Level 0: nothing evaluated, everything type-checked (uc_log's no-log form).
#define KVASIR_DETAIL_CHECK_BIN_0(what, Cmp, opText, a, b, ...)                                  \
    static_cast<void>(                                                                           \
      false ? ::Kvasir::Check::Detail::touch((a), (b)KVASIR_DETAIL_CHECK_USER_ARGS(__VA_ARGS__)) \
            : 0)

#define KVASIR_DETAIL_CHECK_UN_TEXT_2(text, cond, ...)                                            \
    do {                                                                                          \
        if(!static_cast<bool>(cond)) [[unlikely]] {                                               \
            if consteval {                                                                        \
                ::Kvasir::Check::Detail::checkFailedInConstantExpression();                       \
            } else {                                                                              \
                [](auto const&... kvasir_check_extra) __attribute__((cold, noinline, noreturn)) { \
                    KVASIR_DETAIL_CHECK_LOG(crit,                                                 \
                                            KVASIR_DETAIL_CHECK_TEXT(text)                        \
                                              KVASIR_DETAIL_CHECK_USER_FMT(__VA_ARGS__),          \
                                            kvasir_check_extra...);                               \
                    ::Kvasir::Check::Detail::failed();                                            \
                }(KVASIR_DETAIL_CHECK_USER_ARGS_BARE(__VA_ARGS__));                               \
            }                                                                                     \
        }                                                                                         \
    } while(false)

#define KVASIR_DETAIL_CHECK_UN_2(what, cond, ...)                                             \
    KVASIR_DETAIL_CHECK_UN_TEXT_2(what "(" #cond ") failed", cond __VA_OPT__(, ) __VA_ARGS__)

#define KVASIR_DETAIL_CHECK_UN_1(what, cond, ...)                           \
    do {                                                                    \
        if(!static_cast<bool>(cond)) [[unlikely]] {                         \
            if consteval {                                                  \
                ::Kvasir::Check::Detail::checkFailedInConstantExpression(); \
            } else {                                                        \
                ::Kvasir::Check::Detail::failed();                          \
            }                                                               \
        }                                                                   \
    } while(false)

#define KVASIR_DETAIL_CHECK_UN_0(what, cond, ...)                                              \
    static_cast<void>(                                                                         \
      false ? ::Kvasir::Check::Detail::touch((cond)KVASIR_DETAIL_CHECK_USER_ARGS(__VA_ARGS__)) \
            : 0)

#define KVASIR_DETAIL_CHECK_BIN(level, what, Cmp, opText, a, b, ...)                   \
    KVASIR_DETAIL_CHECK_CAT(KVASIR_DETAIL_CHECK_BIN_,                                  \
                            level)(what, Cmp, opText, a, b __VA_OPT__(, ) __VA_ARGS__)
#define KVASIR_DETAIL_CHECK_UN(level, what, cond, ...)                                             \
    KVASIR_DETAIL_CHECK_CAT(KVASIR_DETAIL_CHECK_UN_, level)(what, cond __VA_OPT__(, ) __VA_ARGS__)

#define KVASIR_CHECK(cond, ...)                                                          \
    KVASIR_DETAIL_CHECK_UN(KVASIR_CHECK_LEVEL, "CHECK", cond __VA_OPT__(, ) __VA_ARGS__)
#define KVASIR_CHECK_EQ(a, b, ...) \
    KVASIR_DETAIL_CHECK_BIN(KVASIR_CHECK_LEVEL, "CHECK", Eq, "==", a, b __VA_OPT__(, ) __VA_ARGS__)
#define KVASIR_CHECK_NE(a, b, ...) \
    KVASIR_DETAIL_CHECK_BIN(KVASIR_CHECK_LEVEL, "CHECK", Ne, "!=", a, b __VA_OPT__(, ) __VA_ARGS__)
#define KVASIR_CHECK_LT(a, b, ...)                                                                 \
    KVASIR_DETAIL_CHECK_BIN(KVASIR_CHECK_LEVEL, "CHECK", Lt, "<", a, b __VA_OPT__(, ) __VA_ARGS__)
#define KVASIR_CHECK_LE(a, b, ...) \
    KVASIR_DETAIL_CHECK_BIN(KVASIR_CHECK_LEVEL, "CHECK", Le, "<=", a, b __VA_OPT__(, ) __VA_ARGS__)
#define KVASIR_CHECK_GT(a, b, ...)                                                                 \
    KVASIR_DETAIL_CHECK_BIN(KVASIR_CHECK_LEVEL, "CHECK", Gt, ">", a, b __VA_OPT__(, ) __VA_ARGS__)
#define KVASIR_CHECK_GE(a, b, ...) \
    KVASIR_DETAIL_CHECK_BIN(KVASIR_CHECK_LEVEL, "CHECK", Ge, ">=", a, b __VA_OPT__(, ) __VA_ARGS__)

#define KVASIR_DCHECK(cond, ...)                                                           \
    KVASIR_DETAIL_CHECK_UN(KVASIR_DCHECK_LEVEL, "DCHECK", cond __VA_OPT__(, ) __VA_ARGS__)
#define KVASIR_DCHECK_EQ(a, b, ...)                       \
    KVASIR_DETAIL_CHECK_BIN(KVASIR_DCHECK_LEVEL,          \
                            "DCHECK",                     \
                            Eq,                           \
                            "==",                         \
                            a,                            \
                            b __VA_OPT__(, ) __VA_ARGS__)
#define KVASIR_DCHECK_NE(a, b, ...)                       \
    KVASIR_DETAIL_CHECK_BIN(KVASIR_DCHECK_LEVEL,          \
                            "DCHECK",                     \
                            Ne,                           \
                            "!=",                         \
                            a,                            \
                            b __VA_OPT__(, ) __VA_ARGS__)
#define KVASIR_DCHECK_LT(a, b, ...) \
    KVASIR_DETAIL_CHECK_BIN(KVASIR_DCHECK_LEVEL, "DCHECK", Lt, "<", a, b __VA_OPT__(, ) __VA_ARGS__)
#define KVASIR_DCHECK_LE(a, b, ...)                       \
    KVASIR_DETAIL_CHECK_BIN(KVASIR_DCHECK_LEVEL,          \
                            "DCHECK",                     \
                            Le,                           \
                            "<=",                         \
                            a,                            \
                            b __VA_OPT__(, ) __VA_ARGS__)
#define KVASIR_DCHECK_GT(a, b, ...) \
    KVASIR_DETAIL_CHECK_BIN(KVASIR_DCHECK_LEVEL, "DCHECK", Gt, ">", a, b __VA_OPT__(, ) __VA_ARGS__)
#define KVASIR_DCHECK_GE(a, b, ...)                       \
    KVASIR_DETAIL_CHECK_BIN(KVASIR_DCHECK_LEVEL,          \
                            "DCHECK",                     \
                            Ge,                           \
                            ">=",                         \
                            a,                            \
                            b __VA_OPT__(, ) __VA_ARGS__)

// An expression: true when cond holds; false after the error line (and the policy) when it does not.
#define KVASIR_DETAIL_SOFT_CHECK_2(cond, ...)                                                \
    (__builtin_expect(static_cast<bool>(cond), 1)                                            \
     || ::Kvasir::Check::Detail::softCheckFailed([&]() __attribute__((cold, noinline)) {     \
            KVASIR_DETAIL_CHECK_LOG(error,                                                   \
                                    KVASIR_DETAIL_CHECK_TEXT("SOFT_CHECK(" #cond ") failed") \
                                      KVASIR_DETAIL_CHECK_USER_FMT(__VA_ARGS__)              \
                                        KVASIR_DETAIL_CHECK_USER_ARGS(__VA_ARGS__));         \
        }))
#define KVASIR_DETAIL_SOFT_CHECK_1(cond, ...)            \
    (__builtin_expect(static_cast<bool>(cond), 1)        \
     || ::Kvasir::Check::Detail::softCheckFailed([] {}))
#define KVASIR_DETAIL_SOFT_CHECK_0(cond, ...) static_cast<bool>(cond)
#define KVASIR_SOFT_CHECK(cond, ...)                                             \
    KVASIR_DETAIL_CHECK_CAT(KVASIR_DETAIL_SOFT_CHECK_,                           \
                            KVASIR_CHECK_LEVEL)(cond __VA_OPT__(, ) __VA_ARGS__)

// Logs "unreachable" (and the user part), then panics; at level 0 a plain __builtin_unreachable().
#define KVASIR_DETAIL_CHECK_UNREACHABLE_2(...)                                             \
    KVASIR_DETAIL_CHECK_UN_TEXT_2("UNREACHABLE reached", false __VA_OPT__(, ) __VA_ARGS__)
#define KVASIR_DETAIL_CHECK_UNREACHABLE_1(...) ::Kvasir::Check::Detail::failed()
#define KVASIR_DETAIL_CHECK_UNREACHABLE_0(...) __builtin_unreachable()
#define KVASIR_CHECK_UNREACHABLE(...)                                                          \
    KVASIR_DETAIL_CHECK_CAT(KVASIR_DETAIL_CHECK_UNREACHABLE_, KVASIR_CHECK_LEVEL)(__VA_ARGS__)
