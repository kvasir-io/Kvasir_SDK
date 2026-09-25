#pragma once

#include "uc_log/uc_log.hpp"

#include <cstdint>
#include <string_view>

namespace Kvasir { namespace Ubsan {
    // read over the probe by `kvasir_bench.py ub`, since the log line may be lost
    struct UbsanReports {
        std::uint32_t count;
        std::uint32_t lastReturnAddress;
    };

    [[gnu::used]] inline UbsanReports volatile ubsanReports{};

    // every handler, clang's minimal and gcc's full ABI alike
    inline void countReport(void const* caller) {
        ubsanReports.count = ubsanReports.count + 1;
        ubsanReports.lastReturnAddress
          = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(caller));
    }

    struct UbsanHandlerName {
    private:
        std::string_view sv;

        consteval auto extract_name(std::string_view f) {
            constexpr std::string_view prefix = "__ubsan_handle_";
            constexpr std::string_view suffix = "_minimal";

            if(f.starts_with(prefix)) { f.remove_prefix(prefix.size()); }
            if(f.ends_with(suffix)) { f.remove_suffix(suffix.size()); }
            return f;
        }

    public:
        template<std::convertible_to<std::string_view> S>
        consteval UbsanHandlerName(S const& s) : sv{extract_name(std::string_view{s})} {}

        constexpr operator std::string_view() const { return sv; }
    };
}}   // namespace Kvasir::Ubsan

extern "C" {

#define UBSAN_REPORT()                                                                         \
    do {                                                                                       \
        ::Kvasir::Ubsan::countReport(__builtin_return_address(0));                             \
        [[maybe_unused]] constexpr auto UBSAN_HANDLER_FUNCTION_NAME = __FUNCTION__;            \
        UC_LOG_SCOPE_MODULE("ubsan");                                                          \
        UC_LOG_C(                                                                              \
          "UB: "_sc                                                                            \
            + SC_LIFT(::Kvasir::Ubsan::UbsanHandlerName{SC_LIFT(UBSAN_HANDLER_FUNCTION_NAME)}) \
            + " at {}"_sc,                                                                     \
          __builtin_return_address(0));                                                        \
    } while(false)

[[gnu::used]] inline void __ubsan_handle_mul_overflow_minimal() { UBSAN_REPORT(); }

[[gnu::used]] inline void __ubsan_handle_add_overflow_minimal() { UBSAN_REPORT(); }

[[gnu::used]] inline void __ubsan_handle_alignment_assumption_minimal() { UBSAN_REPORT(); }

[[gnu::used]] inline void __ubsan_handle_sub_overflow_minimal() { UBSAN_REPORT(); }

[[gnu::used]] inline void __ubsan_handle_implicit_conversion_minimal() { UBSAN_REPORT(); }

[[gnu::used]] inline void __ubsan_handle_load_invalid_value_minimal() { UBSAN_REPORT(); }

[[gnu::used]] inline void __ubsan_handle_type_mismatch_minimal() { UBSAN_REPORT(); }

[[gnu::used]] inline void __ubsan_handle_pointer_overflow_minimal() { UBSAN_REPORT(); }

[[gnu::used]] inline void __ubsan_handle_shift_out_of_bounds_minimal() { UBSAN_REPORT(); }

[[gnu::used]] inline void __ubsan_handle_negate_overflow_minimal() { UBSAN_REPORT(); }

[[gnu::used]] inline void __ubsan_handle_builtin_unreachable_minimal() { UBSAN_REPORT(); }

[[gnu::used]] inline void __ubsan_handle_out_of_bounds_minimal() { UBSAN_REPORT(); }

[[gnu::used]] inline void __ubsan_handle_function_type_mismatch_minimal() { UBSAN_REPORT(); }

[[gnu::used]] inline void __ubsan_handle_missing_return_minimal() { UBSAN_REPORT(); }

[[gnu::used]] inline void __ubsan_handle_float_cast_overflow_minimal() { UBSAN_REPORT(); }

[[gnu::used]] inline void __ubsan_handle_divrem_overflow_minimal() { UBSAN_REPORT(); }

[[gnu::used]] inline void __ubsan_handle_invalid_builtin_minimal() { UBSAN_REPORT(); }

[[gnu::used]] inline void __ubsan_handle_nullability_arg_minimal() { UBSAN_REPORT(); }

[[gnu::used]] inline void __ubsan_handle_nonnull_arg_minimal() { UBSAN_REPORT(); }

#undef UBSAN_REPORT
}

#ifndef __clang__
// gcc has no -fsanitize-minimal-runtime: its instrumentation calls the full libubsan ABI, whose
// data block starts with the source location - the only part read here (layouts follow
// gcc/ubsan.cc). Handlers log and return, except the two the compiler declares noreturn, which
// trap.
namespace Kvasir::Ubsan {
struct UbsanSourceLocation {
    char const*   filename;
    std::uint32_t line;
    std::uint32_t column;
};

inline void ubsanReport(std::string_view           check,
                        UbsanSourceLocation const* loc,
                        void*                      caller) {
    countReport(caller);
    // the location pointer is documented as optional
    if(loc == nullptr || loc->filename == nullptr) {
        UC_LOG_C("UB: {} at {}", check, caller);
        return;
    }
    UC_LOG_C("UB: {} at {}:{}:{} (from {})",
             check,
             std::string_view{loc->filename},
             loc->line,
             loc->column,
             caller);
}
}   // namespace Kvasir::Ubsan

extern "C" {
using Kvasir::Ubsan::UbsanSourceLocation;
using ValueHandle = std::uintptr_t;

    #define KVASIR_UBSAN_GCC(name, ...)                                             \
        [[gnu::used]] inline void __ubsan_handle_##name(                            \
          UbsanSourceLocation const* data __VA_OPT__(, ) __VA_ARGS__) {             \
            ::Kvasir::Ubsan::ubsanReport(#name, data, __builtin_return_address(0)); \
        }

KVASIR_UBSAN_GCC(add_overflow,
                 ValueHandle,
                 ValueHandle)
KVASIR_UBSAN_GCC(sub_overflow,
                 ValueHandle,
                 ValueHandle)
KVASIR_UBSAN_GCC(mul_overflow,
                 ValueHandle,
                 ValueHandle)
KVASIR_UBSAN_GCC(negate_overflow,
                 ValueHandle)
KVASIR_UBSAN_GCC(divrem_overflow,
                 ValueHandle,
                 ValueHandle)
KVASIR_UBSAN_GCC(shift_out_of_bounds,
                 ValueHandle,
                 ValueHandle)
KVASIR_UBSAN_GCC(load_invalid_value,
                 ValueHandle)
KVASIR_UBSAN_GCC(out_of_bounds,
                 ValueHandle)
KVASIR_UBSAN_GCC(type_mismatch_v1,
                 ValueHandle)
KVASIR_UBSAN_GCC(vla_bound_not_positive,
                 ValueHandle)
KVASIR_UBSAN_GCC(float_cast_overflow,
                 ValueHandle)
KVASIR_UBSAN_GCC(invalid_builtin)
KVASIR_UBSAN_GCC(nonnull_arg)
KVASIR_UBSAN_GCC(pointer_overflow,
                 ValueHandle,
                 ValueHandle)
KVASIR_UBSAN_GCC(alignment_assumption,
                 ValueHandle,
                 ValueHandle,
                 ValueHandle)

    #undef KVASIR_UBSAN_GCC

// the one handler whose location is the second argument, not the data block
[[gnu::used]] inline void __ubsan_handle_nonnull_return_v1(void const*,
                                                           UbsanSourceLocation const* loc) {
    ::Kvasir::Ubsan::ubsanReport("nonnull_return_v1", loc, __builtin_return_address(0));
}

[[gnu::used,
  noreturn]] inline void
__ubsan_handle_builtin_unreachable(UbsanSourceLocation const* data) {
    ::Kvasir::Ubsan::ubsanReport("builtin_unreachable", data, __builtin_return_address(0));
    while(true) { asm volatile("bkpt 6" : : :); }
}

[[gnu::used,
  noreturn]] inline void
__ubsan_handle_missing_return(UbsanSourceLocation const* data) {
    ::Kvasir::Ubsan::ubsanReport("missing_return", data, __builtin_return_address(0));
    while(true) { asm volatile("bkpt 6" : : :); }
}
}
#endif
