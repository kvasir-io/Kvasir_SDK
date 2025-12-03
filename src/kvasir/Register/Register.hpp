#pragma once
#include "Apply.hpp"
#include "Factories.hpp"
#include "Types.hpp"
#include "kvasir/Mpl/Algorithm.hpp"
#include "kvasir/Mpl/Types.hpp"
#include "kvasir/Mpl/Utility.hpp"

namespace Kvasir { namespace Register {
    // this function produces an MPL::List just like MPL::list, however putting
    // one here allows adl to find it without the user having to write out the
    // whole namespace. Making the list take at least one parameter should
    // prevent ambiguity in a case where MPL::list is also an overload candidate
    template<typename T,
             typename... Ts>
    constexpr brigand::list<T,
                            Ts...>
    list(T,
         Ts...) {
        return brigand::list<T, Ts...>{};
    }

    // factory for compile time values
    template<std::uint32_t I>
    constexpr MPL::Value<std::uint32_t,
                         I>
    value() {
        return MPL::Value<std::uint32_t, I>{};
    }

    template<typename T,
             T I>
    constexpr MPL::Value<T,
                         I>
    value() {
        return MPL::Value<T, I>{};
    }

    // bit helpers
    template<typename Address, int Position, typename TFieldType = bool>
    using RWBitLocT
      = FieldLocation<Address, (1U << unsigned(Position)), ReadWriteAccess, TFieldType>;
    template<typename Address, int Position, typename TFieldType = bool>
    using ROBitLocT
      = FieldLocation<Address, (1U << unsigned(Position)), ReadOnlyAccess, TFieldType>;
    template<typename Address, int Position, typename TFieldType = bool>
    using WOBitLocT
      = FieldLocation<Address, (1U << unsigned(Position)), WriteOnlyAccess, TFieldType>;

    // bit field helpers
    template<typename Address, int HighestBit, int LowestBit, typename TFieldType = unsigned>
    using RWFieldLocT
      = FieldLocation<Address, maskFromRange(HighestBit, LowestBit), ReadWriteAccess, TFieldType>;
    template<typename Address, int HighestBit, int LowestBit, typename TFieldType = unsigned>
    using ROFieldLocT
      = FieldLocation<Address, maskFromRange(HighestBit, LowestBit), ReadOnlyAccess, TFieldType>;
    template<typename Address, int HighestBit, int LowestBit, typename TFieldType = unsigned>
    using WOFieldLocT
      = FieldLocation<Address, maskFromRange(HighestBit, LowestBit), WriteOnlyAccess, TFieldType>;

    template<typename D>
    struct overrideDefaults_impl;

    template<typename... Defaults>
    struct overrideDefaults_impl<brigand::list<Defaults...>> {
        template<typename... T>
        struct AlwaysFalse {
            static constexpr bool value = false;
        };

        template<typename T,
                 typename... Ds>
        static constexpr bool isInDefault() {
            return (std::is_same_v<typename T::location, typename Ds::location> || ...);
        }

        template<typename D,
                 typename... TTs>
        static constexpr auto getDefaultIfNotInTs() {
            using DLoc = typename D::location;
            if constexpr((std::is_same_v<typename TTs::location, DLoc> || ...)) {
                return brigand::list<>{};
            } else {
                return D{};
            }
        }

        template<typename T>
        struct DefaultValues;

        template<typename... Ts>
        struct DefaultValues<brigand::list<Ts...>> {
            static constexpr auto value() {
                if constexpr(
                  requires { (Defaults::isAction, ...); } && requires { (Ts::isAction, ...); })
                {
                    static_assert((isInDefault<Ts, Defaults...>() && ...), "not in default");
                    return list(getDefaultIfNotInTs<Defaults, Ts...>()...);
                } else {
                    static_assert(AlwaysFalse<Defaults...>::value, "not an action");
                }
            }
        };

        template<typename... Ts,
                 typename... Vs>
        static constexpr auto makeList(Ts const&... ts,
                                       brigand::list<Vs...>) {
            return list(Ts{ts}..., Vs{}...);
        }

        template<typename... Ts>
        static constexpr auto value(Ts const&... ts) {
            using FlattenedActions = brigand::flatten<brigand::list<Ts...>>;
            using FlattenedDefaults
              = brigand::flatten<decltype(DefaultValues<FlattenedActions>::value())>;
            return makeList<Ts...>(ts..., FlattenedDefaults{});
        }

        template<typename... Ts>
        static constexpr auto exec(Ts const&... ts) {
            using FlattenedActions = brigand::flatten<brigand::list<Ts...>>;
            using FlattenedDefaults
              = brigand::flatten<decltype(DefaultValues<FlattenedActions>::value())>;
            apply(ts..., FlattenedDefaults{});
        }
    };

    template<typename D>
    struct overrideDefaults;

    template<typename... Defaults>
    struct overrideDefaults<brigand::list<Defaults...>> {
        template<typename... Ts>
        static constexpr auto value(Ts const&... ts) {
            using FlattenedActions = brigand::flatten<brigand::list<Defaults...>>;
            return overrideDefaults_impl<FlattenedActions>::value(ts...);
        }
    };

    template<typename D>
    struct overrideDefaultsRuntime;

    template<typename... Defaults>
    struct overrideDefaultsRuntime<brigand::list<Defaults...>> {
        template<typename... Ts>
        static constexpr void exec(Ts const&... ts) {
            using FlattenedActions = brigand::flatten<brigand::list<Defaults...>>;
            return overrideDefaults_impl<FlattenedActions>::exec(ts...);
        }
    };

}}   // namespace Kvasir::Register

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
#endif
