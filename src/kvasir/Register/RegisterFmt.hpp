#pragma once
#include "Flags.hpp"
#include "Register.hpp"
#include "kvasir/Register/Views.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>
#include <utility>

// The formatters of the generated registers and of Register::Flags<R>. It comes with the log, not with the chip: the
// log row of util.cmake force-includes it into every C++ TU of a log variant (`-include`, next to
// KVASIR_LOG_FORMATTERS), so no chip or driver header names it and a variant without the log never parses it. A
// register is no reflectable struct (svd_converter emits `glaze_reflect = false`): in a TU without this header a
// logged register does not compile ("implicit instantiation of undefined template remote_fmt::formatter<R>") instead
// of printing through a reflection formatter with other text.
//
// KVASIR_LOG_FORMATTERS: set with the log (util.cmake, LOG TRUE) and by host tests that check formatters.
#if defined(KVASIR_LOG_FORMATTERS) && __has_include("remote_fmt/remote_fmt.hpp")
    #include "remote_fmt/remote_fmt.hpp"

template<typename T>
concept PrintableRegister = requires {
    { T::fmt_string } -> std::convertible_to<std::string_view>;
    {
        T::apply_fields([]<typename... Args>(Args&&...) { return true; })
    } -> std::convertible_to<bool>;
};

// A register whose fields also come as data (fields, field_names, field_dims: emitted by the svd
// converter next to fmt_string); the masks are an SDK view on them (Register/Fields.hpp), the value names come with
// each enumerated field's type (kvasirValueNames).
template<typename T>
concept TabledRegister = PrintableRegister<T> && requires {
    typename T::fields;
    { T::field_dims.size() } -> std::convertible_to<std::size_t>;
    requires T::field_names.size() == T::fields::size;
};

// The whole register, one line per field: fmt_string's placeholders filled from one read. Only the raw word goes out;
// the register's type entry (remote_fmt's descriptor `R`: the word's descriptor, fmt_string, the dims, each field's
// mask and its value names) is in the catalog once per image, and the host cuts the word into its fields and names
// them - a number as the field's value, an enumeration as its value's name (the number when the value has none).
namespace Kvasir::Register::Detail {
// The raw word of a register and of its Flags: what format() writes and what the type entry's value descriptor says
using Word           = std::uint32_t;
using WordDescriptor = decltype(remote_fmt::descriptorOf<Word>());

template<typename F>
constexpr void putValueNames(remote_fmt::desc::TextBuilder& b) {
    using E = typename std::remove_cvref_t<F>::DataType;
    if constexpr(requires { kvasirValueNames(E{}); }) {
        constexpr auto names = decltype(kvasirValueNames(E{}))::get();
        b.num(names.size());
        b.put(':');
        for(auto const& n : names) {
            b.num(n.value);
            b.put(',');
            b.counted(n.name);
        }
    } else {
        b.put("0:");
    }
}

template<typename L>
struct FieldsText;

template<typename... Fs>
struct FieldsText<Fields<Fs...>> {
    template<typename R>
    static constexpr void build(remote_fmt::desc::TextBuilder& b) {
        b.num(sizeof...(Fs));
        b.put(':');
        std::size_t i = 0;
        (
          [&] {
              b.num(Kvasir::Register::fieldMasks<R>[i++]);
              b.put(',');
              putValueNames<Fs>(b);
          }(),
          ...);
    }
};

// `R<D><n>:<fmt_string><d>:<dim>,...<f>:<mask>,<k>:<value>,<n>:<name>...`
template<typename R>
struct RegisterText {
    static constexpr void build(remote_fmt::desc::TextBuilder& b) {
        b.put('R');
        b.put(std::string_view{WordDescriptor{}});
        b.counted(std::string_view{R::fmt_string});
        b.num(R::field_dims.size());
        b.put(':');
        for(auto const dim : R::field_dims) {
            b.num(dim);
            b.put(',');
        }
        FieldsText<typename R::fields>::template build<R>(b);
    }
};

template<typename R>
using RegisterTextOf = decltype(remote_fmt::desc::built<RegisterText<R>>());
}   // namespace Kvasir::Register::Detail

template<TabledRegister R>
struct remote_fmt::formatter<R> {
    static consteval auto descriptor() {
        return remote_fmt::typeDescriptor<Kvasir::Register::Detail::RegisterTextOf<R>>();
    }

    template<typename Printer>
    void format(R const&,
                Printer& printer) {
        printer.value(static_cast<Kvasir::Register::Detail::Word>(apply(read(R::FULLREGISTER))));
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

// A register that also carries its fields as data (fields and field_names, emitted by the svd
// converter next to fmt_string; the masks are Register::fieldMasks<T>).
template<typename T>
concept FlagPrintableRegister = PrintableRegister<T> && requires {
    typename T::fields;
    { T::field_names[0] } -> std::convertible_to<std::string_view>;
    requires T::field_names.size() == T::fields::size;
};

    #if REMOTE_FMT_USE_FMT_CHECK
// The host reconstructs the joined flag names, so the spec is checked against a string instead
// of degrading to UncheckedArg.
template<FlagPrintableRegister R>
struct remote_fmt::detail::HostType<Kvasir::Register::Flags<R>> {
    using type = std::string_view;
};
    #endif

namespace Kvasir::Register::Detail {
// `r<D><f>:<mask>,<n>:<name>...`: the single-bit fields (a multi-bit field is a value, not a flag); the host joins the
// names of the set ones with '|', "none" when none is
template<typename R>
struct FlagsText {
    static constexpr void build(remote_fmt::desc::TextBuilder& b) {
        constexpr auto&       masks = Kvasir::Register::fieldMasks<R>;
        constexpr std::size_t flags = [] {
            std::size_t n = 0;
            for(auto const m : Kvasir::Register::fieldMasks<R>) {
                if(std::has_single_bit(m)) { ++n; }
            }
            return n;
        }();
        b.put('r');
        b.put(std::string_view{WordDescriptor{}});
        b.num(flags);
        b.put(':');
        for(std::size_t i = 0; i != masks.size(); ++i) {
            if(std::has_single_bit(masks[i])) {
                b.num(masks[i]);
                b.put(',');
                b.counted(std::string_view{R::field_names[i]});
            }
        }
    }
};

template<typename R>
using FlagsTextOf = decltype(remote_fmt::desc::built<FlagsText<R>>());
}   // namespace Kvasir::Register::Detail

// The raw word; the names are the type entry's.
template<FlagPrintableRegister R>
struct remote_fmt::formatter<Kvasir::Register::Flags<R>> {
    static consteval auto descriptor() {
        return remote_fmt::typeDescriptor<Kvasir::Register::Detail::FlagsTextOf<R>>();
    }

    template<typename Printer>
    constexpr void format(Kvasir::Register::Flags<R> const& flags,
                          Printer&                          printer) const {
        printer.value(static_cast<Kvasir::Register::Detail::Word>(flags.value));
    }
};
#endif
