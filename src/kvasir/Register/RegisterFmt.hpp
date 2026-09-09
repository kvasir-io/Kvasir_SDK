#pragma once
#include "Register.hpp"

#include <bit>
#include <cstddef>
#include <utility>

#if __has_include("remote_fmt/remote_fmt.hpp")
    #include "remote_fmt/remote_fmt.hpp"

    #if __has_include(<aglio/type_descriptor.hpp>)
        #include <aglio/type_descriptor.hpp>
    #endif

template<typename T>
  concept PrintableRegister = requires {
      { T::fmt_string } -> std::convertible_to<std::string_view>;
      {
          T::apply_fields([]<typename... Args>(Args&&...) { return true; })
      } -> std::convertible_to<bool>;
  }
    #if __has_include(<aglio/type_descriptor.hpp>)
  && aglio::Described<T>
    #endif
  ;

template<PrintableRegister R>
struct remote_fmt::formatter<R> {
    template<typename FormatContext>
    auto format(R const&,
                FormatContext& ctx) {
        return R::apply_fields_with_dim([&]<typename... Args>(Args&&... args) {
            return format_to(ctx.out(), SC_LIFT(R::fmt_string), std::forward<Args>(args)...);
        });
    }
};

// A register that also carries its fields as data (field_names/field_masks, emitted by the
// svd converter next to fmt_string).
template<typename T>
concept FlagPrintableRegister = PrintableRegister<T> && requires {
    { T::field_names[0] } -> std::convertible_to<std::string_view>;
    { T::field_masks[0] } -> std::convertible_to<unsigned>;
    requires T::field_names.size() == T::field_masks.size();
};

namespace Kvasir { namespace Register {

    // "What is set", on one line: the single-bit fields present in `value`, named as the
    // register names them -- "arb_lost|abrt_txdata_noack", or "none" when nothing is set.
    //
    //   UC_LOG_W("i2c{} abort {}", instance, Kvasir::Register::Flags<Regs::IC_TX_ABRT_SOURCE>{cause});
    //
    // For status and cause registers: multi-bit fields (counters such as tx_flush_cnt) have no
    // place in a flag list and are skipped. The names never travel -- each set flag costs the
    // two bytes of its catalog id -- so this is what an ISR can afford, unlike the full-register
    // dump the register's own formatter produces (one line per field, every field).
    template<FlagPrintableRegister R>
    struct Flags {
        typename R::Addr::RegType value{};
    };

}}   // namespace Kvasir::Register

    #if REMOTE_FMT_USE_FMT_CHECK
// The host reconstructs the joined flag names, so the spec is checked against a string instead
// of degrading to unchecked_arg.
template<FlagPrintableRegister R>
struct remote_fmt::detail::host_type<Kvasir::Register::Flags<R>> {
    using type = std::string_view;
};
    #endif

// Mirrors remote_fmt's own bitflag enum encoding -- a counted sequence of cataloged names that
// the host joins with '|' -- so no change is needed on the printer side.
template<FlagPrintableRegister R>
struct remote_fmt::formatter<Kvasir::Register::Flags<R>> {
    template<typename Printer>
    constexpr void format(Kvasir::Register::Flags<R> const& flags,
                          Printer&                          printer) const {
        constexpr auto FieldCount = R::field_masks.size();

        auto const isSet = [&](std::size_t i) {
            auto const mask = R::field_masks[i];
            return std::has_single_bit(mask) && (flags.value & mask) != 0;
        };

        std::size_t count = 0;
        for(std::size_t i = 0; i != FieldCount; ++i) {
            if(isSet(i)) { ++count; }
        }

        // The host rejects an empty flag list, so nothing set is reported as a single "none".
        bool const nothingSet = count == 0;
        if(nothingSet) { count = 1; }

        auto const rangeSize = ::remote_fmt::detail::sizeToRangeSize(count);
        printer.printHelper(
          ::remote_fmt::detail::rangeTypeIdentifier<::remote_fmt::detail::RangeType::bitflag,
                                                    ::remote_fmt::detail::RangeLayout::on_ti_each>(
            rangeSize));
        ::remote_fmt::detail::appendSized(rangeSize, count, [&](auto const&... valueArgs) {
            printer.printHelper(valueArgs...);
        });

        if(nothingSet) {
            static constexpr auto none = SC_LIFT("none");
            formatter<std::remove_cvref_t<decltype(none)>>{}.format(none, printer);
            return;
        }

        // The name has to be a compile-time string to be cataloged, so the field index must be
        // one too: the runtime test happens inside the fold, per compile-time index.
        [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            (
              [&] {
                  // A multi-bit field is a value, not a flag: its name never goes out, so it
                  // never reaches the catalog either.
                  if constexpr(std::has_single_bit(R::field_masks[Is])) {
                      if(isSet(Is)) {
                          static constexpr auto name = SC_LIFT(R::field_names[Is]);
                          formatter<std::remove_cvref_t<decltype(name)>>{}.format(name, printer);
                      }
                  }
              }(),
              ...);
        }(std::make_index_sequence<FieldCount>{});
    }
};
#endif
