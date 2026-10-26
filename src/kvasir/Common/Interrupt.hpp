#pragma once

#include "kvasir/Mpl/Types.hpp"

#include <type_traits>

namespace Kvasir { namespace Nvic {
    namespace Action {
        struct Enable {};

        struct Disable {};

        struct Read {};

        struct SetPending {};

        struct ClearPending {};

        template<int I>
        struct SetPriority {};

        static constexpr Enable       enable{};
        static constexpr Disable      disable{};
        static constexpr SetPending   setPending{};
        static constexpr ClearPending clearPending{};
        // other levels: SetPriority<P>{} (makeSetPriority<P>); the core layer rejects a level its priority fields
        // cannot hold, 0 .. 2^bits - 1 for the bits its core.svd implements (Armv6-M: 2, DDI0419E B1.5.4, md
        // l.7093; Armv7-M / Armv8-M Mainline: 3 to 8, DDI0403E Table B1-6, md l.17585, DDI0553B.y RGGCP, md l.4060)
        static constexpr SetPriority<0> setPriority0{};
    }   // namespace Action

    // An interrupt line as a type, read as Index<I>::value (or x.value of a constant such as Interrupt::adc). An
    // MPL::IntegralConstant, but not a runtime int: the deleted conversion hides integral_constant's operator int, so
    // an interrupt index passed where an int is wanted does not compile, and write(field, index) is a type mismatch.
    template<int I>
    struct Index : std::integral_constant<int, I> {
        operator int() const = delete;
    };

    template<typename TAction, typename TIndex>
    struct MakeAction {
        static_assert(MPL::AlwaysFalse<TAction>::value,
                      "could not find this configuration in the included Core");
    };

    template<typename TAction, typename TIndex>
    using MakeActionT = typename MakeAction<TAction, TIndex>::type;

    template<typename TAction,
             typename TIndex>
    constexpr MakeActionT<TAction,
                          TIndex>
    action(TAction,
           TIndex) {
        return {};
    }

    template<int I>
    constexpr auto makeEnable(Index<I>) {
        return MakeActionT<Action::Enable, Index<I>>{};
    }

    template<typename... Ts>
    constexpr auto makeEnable(brigand::list<Ts...>) {
        return MPL::list(makeEnable(Ts{})...);
    }

    template<int I>
    constexpr auto makeDisable(Index<I>) {
        return MakeActionT<Action::Disable, Index<I>>{};
    }

    template<typename... Ts>
    constexpr auto makeDisable(brigand::list<Ts...>) {
        return MPL::list(makeDisable(Ts{})...);
    }

    template<int I>
    constexpr auto makeRead(Index<I>) {
        return MakeActionT<Action::Read, Index<I>>{};
    }

    template<typename... Ts>
    constexpr auto makeRead(brigand::list<Ts...>) {
        return MPL::list(makeRead(Ts{})...);
    }

    template<int I>
    constexpr auto makeSetPending(Index<I>) {
        return MakeActionT<Action::SetPending, Index<I>>{};
    }

    template<typename... Ts>
    constexpr auto makeSetPending(brigand::list<Ts...>) {
        return MPL::list(makeSetPending(Ts{})...);
    }

    template<int I>
    constexpr auto makeClearPending(Index<I>) {
        return MakeActionT<Action::ClearPending, Index<I>>{};
    }

    template<typename... Ts>
    constexpr auto makeClearPending(brigand::list<Ts...>) {
        return MPL::list(makeClearPending(Ts{})...);
    }

    template<int Priority,
             int I>
    constexpr auto makeSetPriority(Index<I>) {
        return MakeActionT<Action::SetPriority<Priority>, Index<I>>{};
    }

    template<int Priority,
             typename... Ts>
    constexpr auto makeSetPriority(brigand::list<Ts...>) {
        return MPL::list(makeSetPriority<Priority>(Ts{})...);
    }

    using IsrFunctionPointer = void (*)();

    // type wrapper around a function pointer including an ISR type
    template<IsrFunctionPointer F, typename T>
    struct Isr;

    template<IsrFunctionPointer F, int I>
    struct Isr<F, Index<I>> {
        static constexpr IsrFunctionPointer value = F;
        using type                                = Isr<F, Index<I>>;
        using IType                               = Index<I>;
    };

    namespace Detail {
        template<IsrFunctionPointer F, typename Indexes>
        struct IsrsForImpl;

        template<IsrFunctionPointer F, typename... Ts>
        struct IsrsForImpl<F, brigand::list<Ts...>> {
            using type = brigand::list<Isr<F, Index<Ts::value>>...>;
        };
    }   // namespace Detail

    /// One Isr entry per interrupt index of a driver, all on one handler:
    /// `using Isr = Nvic::IsrsFor<&onIsr, InterruptIndexs>;`
    /// Indexes may be cv-qualified: a list taken with `decltype` of a
    /// `static constexpr` member is `const brigand::list<...>`.
    template<IsrFunctionPointer F, typename Indexes>
    using IsrsFor = typename Detail::IsrsForImpl<F, std::remove_cvref_t<Indexes>>::type;

    class DefaultIsrs {
    public:
        [[noreturn]] static void onIsr();
    };

    using UnusedIsr = Isr<&DefaultIsrs::onIsr, Index<0>>;

}}   // namespace Kvasir::Nvic
