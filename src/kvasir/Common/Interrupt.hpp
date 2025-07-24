#pragma once

#include "kvasir/Mpl/Types.hpp"
#include "kvasir/Mpl/Utility.hpp"

#include <cassert>

namespace Kvasir { namespace Nvic {
    namespace Action {
        struct Enable {};

        struct Disable {};

        struct Read {};

        struct SetPending {};

        struct ClearPending {};

        template<int I>
        struct SetPriority {};

        struct TriggerInterrupt {};

        static constexpr Enable           enable{};
        static constexpr Disable          disable{};
        static constexpr SetPending       setPending{};
        static constexpr ClearPending     clearPending{};
        static constexpr SetPriority<0>   setPriority0{};
        static constexpr SetPriority<1>   setPriority1{};
        static constexpr SetPriority<2>   setPriority2{};
        static constexpr SetPriority<3>   setPriority3{};
        static constexpr SetPriority<4>   setPriority4{};
        static constexpr SetPriority<5>   setPriority5{};
        static constexpr SetPriority<6>   setPriority6{};
        static constexpr SetPriority<7>   setPriority7{};
        static constexpr TriggerInterrupt triggerInterrupt{};
    }   // namespace Action

    template<int I>
    struct Index {
        static constexpr int value = I;

        constexpr int index() const { return value; }
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

    template<int I>
    constexpr auto makeTriggerInterrupt(Index<I>) {
        return MakeActionT<Action::TriggerInterrupt, Index<I>>{};
    }

    template<typename... Ts>
    constexpr auto makeTriggerInterrupt(brigand::list<Ts...>) {
        return MPL::list(makeTriggerInterrupt(Ts{})...);
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

    class DefaultIsrs {
    public:
        [[noreturn]] static void onIsr() { assert(false); }
    };

    using UnusedIsr = Isr<std::addressof(DefaultIsrs::onIsr), Index<0>>;

    template<typename T = void>
    struct InterruptOffsetTraits;   // must be specialized in a chipxxxInterrupt file

    template<int Priority, int I>
    struct PriorityDisambiguator;   // must be specialized in a chipxxxInterrupt file

}}   // namespace Kvasir::Nvic
