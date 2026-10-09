#pragma once
#include "Register.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <span>
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

// A register whose fields also come as data (field_masks, field_dims, field_name_of: emitted by
// the svd converter next to fmt_string).
template<typename T>
concept TabledRegister = PrintableRegister<T> && requires {
    { T::field_masks[0] } -> std::convertible_to<unsigned>;
    { T::field_dims.size() } -> std::convertible_to<std::size_t>;
    { T::fieldNameOf()[0] } -> std::convertible_to<remote_fmt::catalog_id (*)(unsigned)>;
    requires T::fieldNameOf().size() == T::field_masks.size();
};

// The whole register, one line per field: fmt_string's placeholders filled from one read.
// Table-driven: the dims and the fields go out in a loop
// over field_masks - a number as the field's value, an enumeration as the catalog id of its
// value's name (the number when the value has none) - which is what format_to(fmt_string,
// fields...) sent, for a fraction of the code: that form was a copy of the argument list per
// register, 14 bytes and a stack slot per field (an OUT_BUFFER_CONTROL line 344 bytes).
namespace Kvasir::Register::Detail {
// The field loop, once per Printer: the dims, then each field as a number or as the catalog id
// of its value's name. Not a template on the register, so ten buffer-control registers of
// ten endpoints share it (52 bytes each as a template, measured).
template<typename Printer>
[[gnu::noinline]] void
formatRegisterFields(Printer&                                             printer,
                     std::span<unsigned const>                            dims,
                     unsigned                                             raw,
                     std::span<unsigned const>                            masks,
                     std::span<remote_fmt::catalog_id (*const)(unsigned)> nameOf) {
    for(unsigned const dim : dims) { remote_fmt::formatter<unsigned>{}.format(dim, printer); }
    for(std::size_t i = 0; i != masks.size(); ++i) {
        unsigned const mask  = masks[i];
        unsigned const value = (raw & mask) >> std::countr_zero(mask);
        if(nameOf[i] != nullptr) {
            if(auto const id = nameOf[i](value); id != 0) {
                printer.catalogedString(id);
                continue;
            }
        }
        remote_fmt::formatter<unsigned>{}.format(value, printer);
    }
}
}   // namespace Kvasir::Register::Detail

template<TabledRegister R>
struct remote_fmt::formatter<R> {
    template<typename Printer>
    void format(R const& reg,
                Printer& printer) {
        // Without a catalog (the host tests) a name has no id: the argument list as it is.
        if constexpr(!remote_fmt::use_catalog) {
            R::apply_fields_with_dim([&]<typename... Args>(Args&&... args) {
                return format_to(printer, SC_LIFT(R::fmt_string), std::forward<Args>(args)...);
            });
            return;
        }
        static_cast<void>(reg);
        static constexpr auto nameOfField = R::fieldNameOf();
        printer.beginSub(SC_LIFT(R::fmt_string));
        Kvasir::Register::Detail::formatRegisterFields(
          printer,
          std::span<unsigned const>{R::field_dims},
          static_cast<unsigned>(apply(read(R::FULLREGISTER))),
          std::span<unsigned const>{R::field_masks},
          std::span<remote_fmt::catalog_id (*const)(unsigned)>{nameOfField});
    }
};

// A register that carries only fmt_string and apply_fields (a hand-written one): the argument list
// as it is.
template<PrintableRegister R>
    requires(!TabledRegister<R>)
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

namespace Kvasir::Register::Detail {
// How many single-bit fields are set in `value` (nameId[i] is null for a multi-bit field).
// Not a template: one copy for every register.
[[gnu::noinline]] inline std::size_t
countFlags(unsigned                                     value,
           std::span<unsigned const>                    masks,
           std::span<remote_fmt::catalog_id (*const)()> nameId) {
    std::size_t count = 0;
    for(std::size_t i = 0; i != masks.size(); ++i) {
        if(nameId[i] != nullptr && (value & masks[i]) != 0) { ++count; }
    }
    return count;
}

// The names of the set flags, once per Printer, as formatRegisterFields is for whole registers:
// each as the catalog id of its name. Unrolled per register this was a copy per register *type*
// -- and I2C0's and I2C1's IC_TX_ABRT_SOURCE are two types with the same names.
template<typename Printer>
[[gnu::noinline]] void formatFlagNames(Printer&                                     printer,
                                       unsigned                                     value,
                                       std::span<unsigned const>                    masks,
                                       std::span<remote_fmt::catalog_id (*const)()> nameId) {
    for(std::size_t i = 0; i != masks.size(); ++i) {
        if(nameId[i] != nullptr && (value & masks[i]) != 0) {
            printer.catalogedString(nameId[i]());
        }
    }
}

// The catalog id function of field I's name; null for a multi-bit field, which is a value, not
// a flag: its name never goes out, so it never reaches the catalog either.
template<typename R,
         std::size_t I>
consteval remote_fmt::catalog_id (*flagNameId())() {
    if constexpr(std::has_single_bit(R::field_masks[I])) {
        using Name = std::remove_cvref_t<decltype(SC_LIFT(R::field_names[I]))>;
    #ifdef __clang__
        #pragma clang diagnostic push
        #pragma clang diagnostic ignored "-Wundefined-func-template"
    #endif
        return &remote_fmt::catalog<Name>;
    #ifdef __clang__
        #pragma clang diagnostic pop
    #endif
    } else {
        return nullptr;
    }
}
}   // namespace Kvasir::Register::Detail

// Mirrors remote_fmt's own bitflag enum encoding -- a counted sequence of cataloged names that
// the host joins with '|' -- so no change is needed on the printer side.
template<FlagPrintableRegister R>
struct remote_fmt::formatter<Kvasir::Register::Flags<R>> {
    template<typename Printer>
    constexpr void format(Kvasir::Register::Flags<R> const& flags,
                          Printer&                          printer) const {
        constexpr auto FieldCount = R::field_masks.size();

        if constexpr(remote_fmt::use_catalog) {
            // A constexpr table per register (its names' catalog ids, its masks); the counting
            // and the names are one loop each, shared by every register.
            static constexpr auto nameIds = []<std::size_t... Is>(std::index_sequence<Is...>) {
                return std::array<remote_fmt::catalog_id (*)(), sizeof...(Is)>{
                  Kvasir::Register::Detail::flagNameId<R, Is>()...};
            }(std::make_index_sequence<FieldCount>{});
            static constexpr auto masks = [] {
                std::array<unsigned, R::field_masks.size()> out{};
                for(std::size_t i = 0; i != out.size(); ++i) {
                    out[i] = static_cast<unsigned>(R::field_masks[i]);
                }
                return out;
            }();
            auto const        value = static_cast<unsigned>(flags.value);
            std::size_t const set
              = Kvasir::Register::Detail::countFlags(value,
                                                     std::span<unsigned const>{masks},
                                                     nameIds);
            // The host rejects an empty flag list, so nothing set is reported as a single "none".
            auto const rangeSize = ::remote_fmt::detail::sizeToRangeSize(set == 0 ? 1 : set);
            printer.printHelper(::remote_fmt::detail::rangeTypeIdentifier<
                                ::remote_fmt::detail::RangeType::bitflag,
                                ::remote_fmt::detail::RangeLayout::on_ti_each>(rangeSize));
            ::remote_fmt::detail::appendSized(
              rangeSize,
              set == 0 ? 1 : set,
              [&](auto const&... valueArgs) { printer.printHelper(valueArgs...); });
            if(set == 0) {
                static constexpr auto none = SC_LIFT("none");
                formatter<std::remove_cvref_t<decltype(none)>>{}.format(none, printer);
                return;
            }
            Kvasir::Register::Detail::formatFlagNames(printer,
                                                      value,
                                                      std::span<unsigned const>{masks},
                                                      nameIds);
            return;
        }

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
