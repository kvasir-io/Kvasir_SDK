#pragma once
// The views a generated register derives from its facts (resetValue, fields - Register/Fields.hpp): its default
// values and the read of all its fields. svd_converter emits only one-line members that call these (overrideDefaults
// where every writable field has a defined reset value and every enum field resets to a named value - Register.hpp's
// overrideDefaults -, apply_fields, apply_fields_with_dim), so nothing of them is compiled for a register nobody uses.
// They are member functions of the register rather than a CRTP base: a base would make the register a non-empty
// aggregate for glaze, and aglio::Described (RegisterFmt.hpp's PrintableRegister) would no longer hold.
#include "Fields.hpp"
#include "Register.hpp"

#include <bit>
#include <cstddef>
#include <functional>
#include <type_traits>
#include <utility>

namespace Kvasir::Register {

namespace Detail {
    template<typename F>
    inline constexpr bool defaultWritable = IsWritable<std::remove_cvref_t<F>>::value;

    // the field's reset value from the register's, as the field's own type
    template<typename R, typename F>
    using ResetValueOf = std::integral_constant<typename F::DataType,
                                                static_cast<typename F::DataType>(
                                                  (static_cast<unsigned>(R::resetValue) & F::mask)
                                                  >> std::countr_zero(F::mask))>;

    template<typename R,
             typename F>
    constexpr auto defaultWriteOf() {
        if constexpr(defaultWritable<F>) {
            using Field = std::remove_cvref_t<F>;
            return write(Field{}, ResetValueOf<R, Field>{});
        } else {
            return brigand::list<>{};
        }
    }

    template<typename R, typename L>
    struct DefaultValuesOf;

    template<typename R, typename... Fs>
    struct DefaultValuesOf<R, Fields<Fs...>> {
        using type = brigand::flatten<brigand::list<decltype(defaultWriteOf<R, Fs>())...>>;
    };

    template<typename R, typename L>
    struct ApplyFields;

    template<typename R, typename... Fs>
    struct ApplyFields<R, Fields<Fs...>> {
        template<typename Func,
                 std::size_t... Is>
        [[gnu::always_inline]] static constexpr auto run(Func&& func,
                                                         std::index_sequence<Is...>) {
            if constexpr(sizeof...(Fs) == 0) {
                return std::invoke(std::forward<Func>(func));
            } else {
                auto const regs = apply(read(std::remove_cvref_t<Fs>{}...));
                return std::invoke(
                  std::forward<Func>(func),
                  static_cast<typename std::remove_cvref_t<Fs>::DataType>(get<Is>(regs))...);
            }
        }
    };

    template<typename R>
    struct Dims {
        static constexpr std::size_t count = [] {
            if constexpr(requires { R::field_dims.size(); }) {
                return R::field_dims.size();
            } else {
                return std::size_t{0};
            }
        }();
    };
}   // namespace Detail

/// overrideDefaults' list: one write of the reset value per writable field location of R, in the order of R::fields
/// (a read-only field has no default to write: the hardware ignores it and Register::write refuses it).
template<typename R>
using DefaultValues = typename Detail::DefaultValuesOf<R, typename R::fields>::type;

/// One read of every field of R, and func called with each value as its field's type (an enumerated field as its
/// enum): R::apply_fields.
template<typename R,
         typename Func>
[[gnu::always_inline]] constexpr auto applyFields(Func&& func) {
    using L = typename R::fields;
    return Detail::ApplyFields<R, L>::run(std::forward<Func>(func),
                                          std::make_index_sequence<L::size>{});
}

/// applyFields with the dimensions in front of the fields (a register group's and a dim register's index, from
/// R::field_dims): R::apply_fields_with_dim, the argument list of R::fmt_string.
template<typename R,
         typename Func>
[[gnu::always_inline]] constexpr auto applyFieldsWithDim(Func&& func) {
    return applyFields<R>([&](auto&&... fields) {
        return [&]<std::size_t... Ds>(std::index_sequence<Ds...>) {
            return std::invoke(std::forward<Func>(func), R::field_dims[Ds]..., fields...);
        }(std::make_index_sequence<Detail::Dims<R>::count>{});
    });
}

}   // namespace Kvasir::Register
