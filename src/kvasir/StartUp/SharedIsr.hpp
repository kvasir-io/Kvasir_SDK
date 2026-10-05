#pragma once
// Shared interrupt vectors from declarations.
//
// One NVIC vector often serves several independent sources (the GPIOs of a bank, the EIC lines, the
// doorbells). A driver declares each source it serves as a sub-interrupt:
//
//     using SubIsrs = brigand::list<Nvic::SubIsr<Kvasir::Interrupt::io_bank0 type, &onEdge,
//                                                Nvic::Status<ints_field>,        // pending?
//                                                Nvic::ClearFirst<intr_field>,    // write-one-to-clear
//                                                Nvic::Enable<inte_field>>>;      // set at init
//
// and Startup groups every sub-interrupt of its list by vector into one generated peripheral per
// vector (SharedDispatch): its ISR reads the status registers once, then runs each pending child in
// list order (clear, handler or handler, clear); its init steps write the children's enable bits
// (merged, one write per register) and enable the vector. A vector no child names is neither
// installed nor enabled. A full Isr and a SubIsr on one vector is the existing "two peripherals
// claim one vector" error.
//
// One pass per entry: a child that becomes pending while the pass runs is served by the next entry
// (the line stays asserted). A handler that enables or disables a sibling sees the status read at
// entry. Children are tested one after the other: meant for a handful per vector, not for 48.
#include "kvasir/Common/Interrupt.hpp"
#include "kvasir/Register/Diagnostic.hpp"
#include "kvasir/Register/Register.hpp"

#include <memory>
#include <type_traits>

namespace Kvasir::Nvic {
// Where a child's "this source is pending" bit is. Status<F>: F already includes the source's
// enable (RP's INTS registers). RawStatus<F, E>: F is the raw flag, E the enable bit it is
// ANDed with (SAM's INTFLAG / INTENSET): without it a disabled source would be dispatched.
template<auto Field>
struct Status {};

template<auto Field, auto EnableField>
struct RawStatus {};

// How the pending bit goes away: a written 1 before or after the handler (edge sources, the
// field must be write-one-to-clear), or the handler removes the cause (level sources).
template<auto Field>
struct ClearFirst {};

template<auto Field>
struct ClearLast {};

struct NoClear {};

// The source's enable bit, written once at init; NoEnable when the driver enables the source
// itself at run time.
template<auto Field>
struct Enable {};

struct NoEnable {};

template<typename TIndex,
         auto Fn,
         typename TStatus,
         typename TClear  = NoClear,
         typename TEnable = NoEnable,
         int Priority     = -1>
struct SubIsr {
    using IType   = TIndex;
    using StatusT = TStatus;
    using ClearT  = TClear;
    using EnableT = TEnable;

    static constexpr auto fn       = Fn;
    static constexpr int  priority = Priority;   // -1: the child does not ask for one
};

// Whether a handler template argument is nullptr (a source a driver leaves off). Matched by
// specialization, never as `F == nullptr`: gcc does not fold a function's address compared with
// null to a constant once -fsanitize=null, nonnull-attribute or returns-nonnull-attribute is on
// (all in -fsanitize=undefined; arm-none-eabi-g++ 16.2), or -fno-delete-null-pointer-checks, so
// that comparison breaks a static_assert or a template argument in a sanitized gcc build. clang
// folds it either way.
template<auto F>
inline constexpr bool isNullHandler = false;

template<>
inline constexpr bool isNullHandler<static_cast<void (*)()>(nullptr)> = true;
}   // namespace Kvasir::Nvic

namespace Kvasir::Startup::Detail {
namespace SharedIsrDetail {
    // the list without repeats, first occurrence kept
    template<typename Out, typename In>
    struct Unique;

    template<typename... Os>
    struct Unique<brigand::list<Os...>, brigand::list<>> {
        using type = brigand::list<Os...>;
    };

    template<typename... Os, typename H, typename... Ts>
    struct Unique<brigand::list<Os...>, brigand::list<H, Ts...>>
      : Unique<std::conditional_t<(std::is_same_v<H, Os> || ...),
                                  brigand::list<Os...>,
                                  brigand::list<Os..., H>>,
               brigand::list<Ts...>> {};

    template<typename L>
    using UniqueT = typename Unique<brigand::list<>, L>::type;

    template<auto F>
    using FieldOf = std::remove_cvref_t<decltype(F)>;

    template<typename S>
    struct StatusFields;

    template<auto F>
    struct StatusFields<Nvic::Status<F>> {
        using type = brigand::list<FieldOf<F>>;
    };

    template<auto F, auto E>
    struct StatusFields<Nvic::RawStatus<F, E>> {
        using type = brigand::list<FieldOf<F>, FieldOf<E>>;
    };

    template<typename Field,
             typename Tuple>
    [[gnu::always_inline]] inline bool isSet(Tuple const& st) {
        return static_cast<unsigned>(st[Field{}]) != 0U;
    }

    template<typename S, typename Tuple>
    struct Pending;

    template<auto F, typename Tuple>
    struct Pending<Nvic::Status<F>, Tuple> {
        [[gnu::always_inline]] static bool get(Tuple const& st) { return isSet<FieldOf<F>>(st); }
    };

    template<auto F, auto E, typename Tuple>
    struct Pending<Nvic::RawStatus<F, E>, Tuple> {
        [[gnu::always_inline]] static bool get(Tuple const& st) {
            return isSet<FieldOf<F>>(st) && isSet<FieldOf<E>>(st);
        }
    };

    // A literal write of every bit of the field. On a register whose other bits are all in its
    // write-ignored-if-zero mask that is a single store.
    template<typename Field>
    constexpr auto writeOnes() {
        constexpr unsigned mask = Register::Detail::GetMask<Field>::value;
        return Register::Action<Field, Register::WriteLiteralAction<mask>>{};
    }

    template<typename Field>
    constexpr bool isWriteOneToClear() {
        using A                 = Register::Detail::GetAddress<Field>;
        constexpr unsigned mask = Register::Detail::GetMask<Field>::value;
        return (A::writeIgnoredIfZeroMask & mask) == mask;
    }

    template<typename C>
    struct Clear {
        [[gnu::always_inline]] static void before() {}

        [[gnu::always_inline]] static void after() {}
    };

    template<auto F>
    struct Clear<Nvic::ClearFirst<F>> {
        KVASIR_STATIC_ASSERT((isWriteOneToClear<FieldOf<F>>()),
                             (::Kvasir::Register::Diagnostic::NotWriteOneToClear<
                               ::Kvasir::Register::Detail::GetAddress<FieldOf<F>>,
                               FieldOf<F>::Mask,
                               'F'>),
                             "ClearFirst names a field that is not write-one-to-clear in the "
                             "generated header (its bits are not in the register's "
                             "write-ignored-if-zero mask): writing 1 to it would not clear it, and "
                             "the write would not be a single store");

        [[gnu::always_inline]] static void before() { Register::apply(writeOnes<FieldOf<F>>()); }

        [[gnu::always_inline]] static void after() {}
    };

    template<auto F>
    struct Clear<Nvic::ClearLast<F>> {
        KVASIR_STATIC_ASSERT(
          (isWriteOneToClear<FieldOf<F>>()),
          (::Kvasir::Register::Diagnostic::NotWriteOneToClear<
            ::Kvasir::Register::Detail::GetAddress<FieldOf<F>>,
            FieldOf<F>::Mask,
            'L'>),
          "ClearLast names a field that is not write-one-to-clear in the generated "
          "header (its bits are not in the register's write-ignored-if-zero mask): "
          "writing 1 to it would not clear it, and the write would not be a single "
          "store");

        [[gnu::always_inline]] static void before() {}

        [[gnu::always_inline]] static void after() { Register::apply(writeOnes<FieldOf<F>>()); }
    };

    template<typename E>
    struct EnableSteps {
        using type = brigand::list<>;
    };

    template<auto F>
    struct EnableSteps<Nvic::Enable<F>> {
        using type = brigand::list<decltype(writeOnes<FieldOf<F>>())>;
    };

    template<typename... Fields>
    [[gnu::always_inline]] inline auto readAll(brigand::list<Fields...>) {
        return Register::apply(Register::read(Fields{})...);
    }

    // the priority the children ask for: -1 none, -2 children that disagree
    template<typename... Subs>
    constexpr int agreedPriority() {
        int p = -1;
        for(int const q : {Subs::priority...}) {
            if(q == -1) { continue; }
            if(p == -1) {
                p = q;
            } else if(p != q) {
                return -2;
            }
        }
        return p;
    }

    template<typename TIndex, int P>
    struct PrioritySteps {
        using type = brigand::list<decltype(Nvic::makeSetPriority<P>(TIndex{}))>;
    };

    template<typename TIndex>
    struct PrioritySteps<TIndex, -1> {
        using type = brigand::list<>;
    };
}   // namespace SharedIsrDetail

// The generated peripheral for one vector. An ordinary Startup entry: an isr, init steps.
template<typename TIndex, typename... Subs>
struct SharedDispatch {
    static_assert(sizeof...(Subs) > 0);
    static_assert(SharedIsrDetail::agreedPriority<Subs...>() != -2,
                  "the sub-interrupts of one vector ask for different priorities");

    using StatusFieldTypes = SharedIsrDetail::UniqueT<
      brigand::append<typename SharedIsrDetail::StatusFields<typename Subs::StatusT>::type...>>;

    using Tuple = decltype(SharedIsrDetail::readAll(StatusFieldTypes{}));

    template<typename Sub>
    [[gnu::always_inline]] static void one(Tuple const& st) {
        if(SharedIsrDetail::Pending<typename Sub::StatusT, Tuple>::get(st)) {
            SharedIsrDetail::Clear<typename Sub::ClearT>::before();
            Sub::fn();
            SharedIsrDetail::Clear<typename Sub::ClearT>::after();
        }
    }

    static void onIsr() {
        auto const st = SharedIsrDetail::readAll(StatusFieldTypes{});
        (one<Subs>(st), ...);
    }

    static constexpr Nvic::Isr<std::addressof(onIsr), TIndex> isr{};

    static constexpr auto initStepInterruptConfig = MPL::list(
      brigand::append<typename SharedIsrDetail::EnableSteps<typename Subs::EnableT>::type...,
                      typename SharedIsrDetail::
                        PrioritySteps<TIndex, SharedIsrDetail::agreedPriority<Subs...>()>::type>{},
      Nvic::makeClearPending(TIndex{}));
    static constexpr auto initStepPeripheryEnable = MPL::list(Nvic::makeEnable(TIndex{}));
};

// ---- grouping: every SubIsr of a list, one SharedDispatch per distinct vector, list order ----

template<typename T>
struct SubIsrsOf {
    using type = brigand::list<>;
};

template<typename T>
    requires requires { typename T::SubIsrs; }
struct SubIsrsOf<T> {
    using type = typename T::SubIsrs;
};

template<typename Index, typename Subs>
struct DispatchFor;

template<typename Index, typename... Subs>
struct DispatchFor<Index, brigand::list<Subs...>> {
    using type = SharedDispatch<Index, Subs...>;
};

template<typename Index, typename Sub>
using OnIndex = std::is_same<Index, typename Sub::IType>;

template<typename Index, typename AllSubs>
struct SubsOnIndex;

template<typename Index, typename... Subs>
struct SubsOnIndex<Index, brigand::list<Subs...>> {
    using type = brigand::append<
      std::conditional_t<OnIndex<Index, Subs>::value, brigand::list<Subs>, brigand::list<>>...>;
};

template<typename AllSubs, typename Indexes>
struct DispatchersOf;

template<typename AllSubs, typename... Indexes>
struct DispatchersOf<AllSubs, brigand::list<Indexes...>> {
    using type = brigand::list<
      typename DispatchFor<Indexes, typename SubsOnIndex<Indexes, AllSubs>::type>::type...>;
};

template<typename Subs>
struct ITypesOf;

template<typename... Subs>
struct ITypesOf<brigand::list<Subs...>> {
    using type = brigand::list<typename Subs::IType...>;
};

template<typename List>
struct SharedDispatchers;

template<typename... Ps>
struct SharedDispatchers<brigand::list<Ps...>> {
    using AllSubs = brigand::append<brigand::list<>, typename SubIsrsOf<Ps>::type...>;
    using Indexes = SharedIsrDetail::UniqueT<typename ITypesOf<AllSubs>::type>;
    using type    = typename DispatchersOf<AllSubs, Indexes>::type;
};

// The list Startup works on: the user's peripherals, then the generated dispatchers. Without a
// SubIsrs anywhere it is the user's list unchanged.
template<typename... Ps>
using WithSharedDispatchers
  = brigand::append<brigand::list<Ps...>, typename SharedDispatchers<brigand::list<Ps...>>::type>;
}   // namespace Kvasir::Startup::Detail
