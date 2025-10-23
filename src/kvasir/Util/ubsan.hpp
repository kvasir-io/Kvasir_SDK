#pragma once

namespace kvasir { namespace detail {
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
}}   // namespace kvasir::detail

extern "C" {

#define UBSAN_REPORT()                                                                          \
    do {                                                                                        \
        [[maybe_unused]] constexpr auto UBSAN_HANDLER_FUNCTION_NAME = __FUNCTION__;             \
        UC_LOG_C(                                                                               \
          "UB: "_sc                                                                             \
            + SC_LIFT(::kvasir::detail::UbsanHandlerName{SC_LIFT(UBSAN_HANDLER_FUNCTION_NAME)}) \
            + " at {}"_sc,                                                                      \
          __builtin_return_address(0));                                                         \
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

#undef UBSAN_REPORT
}
