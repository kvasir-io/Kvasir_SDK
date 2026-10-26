#pragma once

// Hardware resources a peripheral provides or claims, checked across the whole image.
//
// A peripheral in a Startup list may expose
//
//     using Provides = brigand::list<Resource<Tag, Id...>...>;   // what it configures and owns
//     using Claims   = brigand::list<Resource<Tag, Id...>...>;   // what it uses and needs configured
//
// where `Tag` names a kind of resource (a DMA channel, a GPIO, a clock, ...) and the ids
// the one instance of it. Both are optional; absent means empty. On top of what a
// peripheral says, Startup derives provides and claims from what it does: every action in
// `powerClockEnable` and `initStepPinConfig` is passed through `ResourceOfAction`, which
// the chip layer specialises for the register writes that mean "this peripheral now owns
// GPIO n" or "this block is out of reset"; every action of the later phases goes through
// `ClaimOfAction`, specialised for "this write needs block b out of reset". A pin is
// therefore provided by whoever configures it, and a block claimed by whoever writes it,
// with no declaration anywhere.
//
// `Startup` collects all of it from its own list and from every `SecondaryCore` in it and
// refuses to build unless (each rule is an offender list in Diagnostics below; Checks.hpp's
// ImageChecks asserts them, one report per offender):
//
//   1. no resource is claimed by two peripherals (`noDoubleClaim`): two drivers on one DMA
//      channel overwrite each other's callback and registers;
//   2. no resource is provided by two peripherals (`noDoubleProvide`): two DmaBase
//      instances on one interrupt line clobber each other's INTE mask, two peripherals that
//      configure one pin leave it however the second one wanted;
//   3. every claimed resource is provided somewhere (`claimsProvided`): a DmaBase that is
//      in no Startup list never enables its interrupt line, a pin handed to a driver that
//      nobody configured stays isolated;
//   4. the provider sits in the same core's list as the claimer (`claimsLocal`): a DMA
//      instance on core 1 runs its completion callbacks on core 1, so a driver on core 0
//      that uses one of its channels has its callback on the wrong core;
//   5. a peripheral that says which core it belongs to is in that core's list
//      (`coreAffinityHonoured`): a GPIO interrupt enabled in one core's mask and the other
//      core's NVIC fires as unhandled on the one and never reaches the other.
//
// Not every kind of resource wants every rule, so a tag may carry policies, all optional:
//
//     struct PinTag {
//         static constexpr bool sharedClaim      = true;    // several claimers are fine (default false)
//         static constexpr bool coreLocal        = false;   // rule 4 does not apply (default true)
//         static constexpr bool mergeIdentical   = false;   // rule 2: same key, same payload is one
//                                                           // provider, same key, other payload two
//                                                           // (default false: any second provider fails)
//         static constexpr bool optionalProvider = false;   // rule 3 only once something provides
//                                                           // this tag at all (default false)
//         static constexpr unsigned keyArity     = 2;       // leading ids that name the hardware
//                                                           // (default 1)
//         static constexpr char const* describeAs = "DMA channel";  // the name an error uses
//                                                           // (default: the tag's type name)
//         static constexpr bool describeHex      = true;    // the error prints the first id as
//                                                           // an address (default false: decimal)
//     };
//
// A resource's identity is its whole type, `Resource<Tag, Ids...>`; its *key* is the tag
// and the first id. The extra ids are payload: a PWM slice is `Resource<PwmSliceTag, slice,
// div, top>`, and two providers of slice 3 merge if they agree on div and top (that tag says
// mergeIdentical) and collide if they do not. A clock is `Resource<ClkPeri, hertz>`; every
// driver that runs from it claims it at the speed its config says, and a driver whose
// number is not the one the clock settings provide claims something nothing provides.
//
// Identities are keyed by the hardware (channel 2, GPIO 19, slice 3), not by the type that
// provides it, which is what makes 2 fall out for two peripherals that overlap without
// either of them knowing about the other.
//
// This header is pure type computation and compiles on the host; the checks are unit
// tested in tests/resources_tests.cpp.

#include "kvasir/Mpl/List.hpp"
#include "kvasir/Mpl/Types.hpp"
#include "kvasir/StartUp/Entry.hpp"
#include "kvasir/StartUp/InterruptNames.hpp"
#include "kvasir/Util/Config.hpp"
#include "kvasir/Util/Diagnostic.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>
#include <utility>

namespace Kvasir { namespace Startup {

    namespace Detail {
        template<typename Tag, unsigned long long... Ids>
        struct ResourceT {
            using tag                          = Tag;
            using type                         = ResourceT<Tag, Ids...>;
            static constexpr std::size_t arity = sizeof...(Ids);
        };
    }   // namespace Detail

    // Ids are normalised to one integer type on the way in, so `Resource<Tag, 2>`,
    // `Resource<Tag, 2u>` and `Resource<Tag, Channel::ch2>` are the same resource: an
    // `auto` parameter would otherwise make the literal's type part of the identity.
    template<typename Tag, auto... Ids>
    using Resource = Detail::ResourceT<Tag, static_cast<unsigned long long>(Ids)...>;

    // A resource's tag and ids, read without naming Detail::ResourceT (chip and HAL code use
    // this; only the SDK's own rules match the Detail type). `ResourceIds<R>` of anything that
    // is not a Resource is incomplete, so a wrong argument is a compile error.
    template<typename R>
    struct ResourceIds;

    template<typename Tag, unsigned long long... Ids>
    struct ResourceIds<Detail::ResourceT<Tag, Ids...>> {
        using tag = Tag;
        static constexpr std::array<unsigned long long, sizeof...(Ids)> ids{Ids...};
    };

    // true when R is a Resource whose tag is Tag: `isResourceOf<Clk<ClkPeri, 1>, ClkPeri>`.
    template<typename R, typename Tag>
    inline constexpr bool isResourceOf = false;

    template<typename Tag, unsigned long long... Ids>
    inline constexpr bool isResourceOf<Detail::ResourceT<Tag, Ids...>, Tag> = true;

    // What an init-step action provides. The chip layer specialises this for the register
    // writes that mean ownership (a GPIO's function select, for one); everything else
    // provides nothing. Actions come from `initStepPinConfig`.
    template<typename Action>
    struct ResourceOfAction {
        using type = brigand::list<>;
    };

    // What an init-step action claims: the counterpart of ResourceOfAction for the later
    // phases (pin, periphery, interrupt, enable). The chip layer specialises it for the
    // register writes that need something to have happened first - a write into a block
    // that a `powerClockEnable` has to take out of reset - and Startup requires a provider
    // for every claim found, exactly as for a declared `Claims`.
    template<typename Action>
    struct ClaimOfAction {
        using type = brigand::list<>;
    };

    // What an init-step action enables: the interrupt indexes (as std::integral_constant<int,
    // I>) a literal write turns on. The core layer specialises this for the NVIC's ISER
    // registers and SysTick's TICKINT (cortex_common's Nvic.hpp and Systick.hpp); everything
    // else enables nothing. Actions come from `initStepInterruptConfig` and
    // `initStepPeripheryEnable`, and Startup requires an Isr in the same list for every index
    // found (ListRules::EnabledLinesHandled).
    template<typename Action>
    struct InterruptOfAction {
        using type = brigand::list<>;
    };

    // What an init-step action sets as an interrupt's priority: IsrPriority<I, L> for a write
    // that sets index I to level L. The core layer specialises this for the NVIC's IPR and the
    // SCB's SHPR bytes (cortex_common); everything else sets none. A write whose level is not
    // known at compile time (a run-time value, a toggle, a literal over part of the priority
    // bits) sets unknownIsrLevel, which fails an ISR-context contract instead of passing it
    // by accident. ListRules::IsrPrioritiesIn pairs every enabled index with its level, 0 when
    // unset.
    inline constexpr int unknownIsrLevel = -1;

    template<int Index, int Level>
    struct IsrPriority {
        static constexpr int index = Index;
        static constexpr int level = Level;
    };

    template<typename Action>
    struct PriorityOfAction {
        using type = brigand::list<>;
    };

    // The clock a core runs on, named here because the chip-agnostic core layer (SysTick)
    // claims it; the chip's clock settings provide it next to their own clocks (RP
    // DefaultProvides, the SAM ClockTree, the STM32 Rcc::ClockTree, or a hand-written
    // ClockSettings' `Startup::ProcessorClock<Hz>`). Mandatory like every clock: an image with
    // a SysTick and clock settings that do not state the processor clock does not build.
    struct ProcessorClockTag {
        static constexpr bool sharedClaim = true;
        static constexpr bool coreLocal   = false;
    };

    template<auto Hz>
    using ProcessorClock = Resource<ProcessorClockTag, Hz>;

    // ---- dependencies between listed peripherals ---------------------------------------
    // Some peripherals need *another listed type* on a particular core: ClockSync reads its
    // Reference on core 0 and its Target on its own core, LaunchTimeout its clock on core 0.
    // Every entry of every list implicitly provides the three resources below (ResourceCheck
    // adds them), so a peripheral just claims what it needs and rules 3 and 4 do the rest:
    //
    //   Listed<T>         T is in some list
    //   ListedOn<T, N>    T is in core N's list
    //   ListedHere<T>     T is in the claimer's own list (a core-local tag)
    //
    // Startup reports these claims with their own message (dependenciesListed), and checks
    // that a ListedOn<T, 0> claimed from a SecondaryCore's list names a T that precedes the
    // SecondaryCore, so its init has run by launch (ListRules::DependenciesPrecedeLaunch).
    // mergeIdentical: the same type twice in one list is Startup's "listed twice" finding,
    // not a double provide here.
    template<typename T>
    struct ListedTag {
        static constexpr bool sharedClaim    = true;
        static constexpr bool coreLocal      = false;
        static constexpr bool mergeIdentical = true;
    };

    template<typename T>
    struct ListedOnTag {
        static constexpr bool sharedClaim    = true;
        static constexpr bool coreLocal      = false;
        static constexpr bool mergeIdentical = true;
    };

    template<typename T>
    struct ListedHereTag {
        static constexpr bool sharedClaim    = true;
        static constexpr bool mergeIdentical = true;
    };

    template<typename T>
    using Listed = Resource<ListedTag<T>>;

    template<typename T, auto Core>
    using ListedOn = Resource<ListedOnTag<T>, Core>;

    template<typename T>
    using ListedHere = Resource<ListedHereTag<T>>;

    namespace Detail {
        // ---- tag policies, with their defaults ------------------------------------------
        template<typename Tag>
        constexpr bool sharedClaim = static_cast<bool>(KVASIR_CONFIG_OR(Tag, sharedClaim, false));

        template<typename Tag>
        constexpr bool coreLocal = static_cast<bool>(KVASIR_CONFIG_OR(Tag, coreLocal, true));

        template<typename Tag>
        constexpr bool mergeIdentical
          = static_cast<bool>(KVASIR_CONFIG_OR(Tag, mergeIdentical, false));

        // How many leading ids form the key. One by default; a GPIO is (port, pin).
        template<typename Tag>
        constexpr std::size_t keyArity
          = static_cast<std::size_t>(KVASIR_CONFIG_OR(Tag, keyArity, std::size_t{1}));

        template<typename Tag>
        constexpr bool optionalProvider
          = static_cast<bool>(KVASIR_CONFIG_OR(Tag, optionalProvider, false));

        // ---- keys ----------------------------------------------------------------------
        // Same tag and same first id (or both without one): the same piece of hardware,
        // whatever the payload says about it.
        template<std::size_t K,
                 std::size_t NA,
                 std::size_t NB>
        constexpr bool sameLeading(std::array<unsigned long long,
                                              NA> const& a,
                                   std::array<unsigned long long,
                                              NB> const& b) {
            constexpr std::size_t shorter = NA < NB ? NA : NB;
            for(std::size_t i = 0; i < (K < shorter ? K : shorter); ++i) {
                if(a[i] != b[i]) { return false; }
            }
            // a key that ends before K: the same only if the other ends there too
            return K <= shorter || NA == NB;
        }

        template<typename A, typename B>
        struct SameKey : std::false_type {};

        template<typename Tag, unsigned long long... As, unsigned long long... Bs>
        struct SameKey<ResourceT<Tag, As...>, ResourceT<Tag, Bs...>>
          : std::bool_constant<sameLeading<keyArity<Tag>>(
              std::array<unsigned long long, sizeof...(As)>{As...},
              std::array<unsigned long long, sizeof...(Bs)>{Bs...})> {};

        template<typename A, typename B>
        struct SameTag : std::false_type {};

        template<typename Tag, unsigned long long... As, unsigned long long... Bs>
        struct SameTag<ResourceT<Tag, As...>, ResourceT<Tag, Bs...>> : std::true_type {};

        // ---- what a peripheral says ----------------------------------------------------
        template<typename T>
        struct ExplicitProvides {
            using type = brigand::list<>;
        };

        template<typename T>
            requires requires { typename T::Provides; }
        struct ExplicitProvides<T> {
            using type = typename T::Provides;
        };

        template<typename T>
        struct ClaimsOf {
            using type = brigand::list<>;
        };

        template<typename T>
            requires requires { typename T::Claims; }
        struct ClaimsOf<T> {
            using type = typename T::Claims;
        };

        // ---- what a peripheral does ----------------------------------------------------
        // Its phase members, through Entry.hpp's PhaseActionsOf (one action or a list of
        // actions and lists, flattened).
        template<typename List>
        struct ResourcesOfActions;

        template<typename... As>
        struct ResourcesOfActions<brigand::list<As...>> {
            using type = brigand::flatten<brigand::list<typename ResourceOfAction<As>::type...>>;
        };

        // Provided by doing: pins from initStepPinConfig, reset releases from
        // powerClockEnable (both through ResourceOfAction).
        template<typename T>
        struct DerivedProvides {
            using Actions = brigand::append<typename PhaseActionsOf<T, Phase::PowerClock>::type,
                                            typename PhaseActionsOf<T, Phase::Pins>::type>;
            using type    = typename ResourcesOfActions<Actions>::type;
        };

        template<typename List>
        struct ClaimsOfActions;

        template<typename... As>
        struct ClaimsOfActions<brigand::list<As...>> {
            using type = brigand::flatten<brigand::list<typename ClaimOfAction<As>::type...>>;
        };

        // Claimed by doing: every action of the phases after powerClockEnable, through
        // ClaimOfAction (a write into a block that has to be out of reset by then).
        template<typename T>
        struct DerivedClaims {
            using Actions = brigand::append<typename PhaseActionsOf<T, Phase::Pins>::type,
                                            typename PhaseActionsOf<T, Phase::Periphery>::type,
                                            typename PhaseActionsOf<T, Phase::Interrupts>::type,
                                            typename PhaseActionsOf<T, Phase::Enable>::type>;
            using type    = typename ClaimsOfActions<Actions>::type;
        };

        // ---- list helpers --------------------------------------------------------------
        template<typename R,
                 typename... Ts>
        constexpr std::size_t countKeyIn(brigand::list<Ts...>) {
            return (std::size_t{0} + ... + std::size_t{SameKey<R, Ts>::value});
        }

        template<typename R,
                 typename... Ts>
        constexpr std::size_t countTagIn(brigand::list<Ts...>) {
            return (std::size_t{0} + ... + std::size_t{SameTag<R, Ts>::value});
        }

        // Every provider with R's key is R itself: what mergeIdentical accepts.
        template<typename R,
                 typename... Ts>
        constexpr bool keyAgrees(brigand::list<Ts...>) {
            return ((!SameKey<R, Ts>::value || std::is_same_v<R, Ts>) && ...);
        }

        // MPL::Dedup: the list without repeats, first occurrence kept. A peripheral that writes one pin's
        // registers several times in its own init list provides that pin once.
        template<typename T>
        struct ProvidesOf {
            using type = MPL::Dedup<brigand::append<typename ExplicitProvides<T>::type,
                                                    typename DerivedProvides<T>::type>>;
        };

        // A peripheral's claims: what it says, as said, plus what its init steps imply (a
        // block written several times is claimed once).
        template<typename T>
        struct AllClaimsOf {
            using type = brigand::append<typename ClaimsOf<T>::type,
                                         MPL::Dedup<typename DerivedClaims<T>::type>>;
        };

        // ---- what the rules read -------------------------------------------------------
        // A peripheral with `static constexpr unsigned startupCore = N;` belongs in list N.
        template<typename T>
        constexpr int startupCoreOf = static_cast<int>(KVASIR_CONFIG_OR(T, startupCore, -1));

        template<typename List>
        struct ProvidesIn;

        template<typename... Ps>
        struct ProvidesIn<brigand::list<Ps...>> {
            using type = brigand::flatten<brigand::list<typename ProvidesOf<Ps>::type...>>;
        };

        template<typename List>
        struct ClaimsIn;

        template<typename... Ps>
        struct ClaimsIn<brigand::list<Ps...>> {
            using type = brigand::flatten<brigand::list<typename AllClaimsOf<Ps>::type...>>;
        };

        // What list N provides by its entries alone (the Listed* resources above).
        template<std::size_t N, typename List>
        struct ImplicitProvides;

        template<std::size_t N, typename... Ps>
        struct ImplicitProvides<N, brigand::list<Ps...>> {
            using type = brigand::list<Listed<Ps>..., ListedOn<Ps, N>..., ListedHere<Ps>...>;
        };

        template<std::size_t N, typename List>
        struct ProvidesInAt {
            using type = brigand::append<typename ProvidesIn<List>::type,
                                         typename ImplicitProvides<N, List>::type>;
        };

        template<typename R>
        struct IsListedClaim : std::false_type {};

        template<typename T, unsigned long long... Ids>
        struct IsListedClaim<ResourceT<ListedTag<T>, Ids...>> : std::true_type {};

        template<typename T, unsigned long long... Ids>
        struct IsListedClaim<ResourceT<ListedOnTag<T>, Ids...>> : std::true_type {};

        template<typename T, unsigned long long... Ids>
        struct IsListedClaim<ResourceT<ListedHereTag<T>, Ids...>> : std::true_type {};

        // The list without its Listed* resources.
        template<typename List>
        using DropListed = MPL::Filter<MPL::NotP<MPL::Holds<IsListedClaim>>, List>;

        // The sets every rule reads, over one image's lists (core 0 first).
        template<typename Seq, typename... CoreLists>
        struct ResourceSets;

        template<std::size_t... Is, typename... CoreLists>
        struct ResourceSets<std::index_sequence<Is...>, CoreLists...> {
            using Lists = brigand::list<CoreLists...>;
            using AllProvides
              = brigand::flatten<brigand::list<typename ProvidesInAt<Is, CoreLists>::type...>>;
            using AllClaims
              = brigand::flatten<brigand::list<typename ClaimsIn<CoreLists>::type...>>;

            template<std::size_t N>
            using ProvidesAt = typename ProvidesInAt<N, brigand::at_c<Lists, N>>::type;
        };
    }   // namespace Detail

    // ---- diagnostics ---------------------------------------------------------------------
    // A rule that fails is reported through a Report<Rule, Peripheral, Resource> whose
    // static_assert fires inside its own instantiation, so the compiler's "in instantiation
    // of" trail names which peripheral and which resource - not just that some rule failed.
    // The offenders of each rule are computed here as (Peripheral, Resource) pairs, which a
    // test can check on the host; Startup instantiates the reports (Diagnose below). A tag
    // may add `static constexpr char const* message` to say what its collision means.
    namespace Diagnostics {
        template<typename Peripheral, typename Resource>
        struct Pair {};

        // A rule holds when its offender list is empty.
        template<typename Offenders>
        inline constexpr bool none = brigand::size<Offenders>::value == 0;

        template<typename Tag>
        constexpr std::string_view tagMessage
          = std::string_view{KVASIR_CONFIG_OR(Tag, message, std::string_view{})};

        template<typename R>
        struct TagOf {
            using type = void;
        };

        template<typename Tag, unsigned long long... Ids>
        struct TagOf<Detail::ResourceT<Tag, Ids...>> {
            using type = Tag;
        };

        // What a rule's number is (Extra = std::integral_constant<int, N>): a rule says so with
        // `static constexpr NumberKind kind`.
        enum class NumberKind : std::uint8_t { plain, interrupt, core };

        template<typename Rule>
        constexpr NumberKind numberKindOf = KVASIR_CONFIG_OR(Rule, kind, NumberKind::plain);

        // ": Io pin 0.25", ": interrupt 37 (I2C1_IRQ)", ": core 1"
        template<typename Rule, typename Extra>
        struct DescribeExtra {
            static constexpr void into(Kvasir::Diagnostic::Text& t) {
                if constexpr(!std::is_void_v<Extra>) {
                    t << ": ";
                    Kvasir::Diagnostic::appendType<Extra>(t);
                }
            }
        };

        constexpr void appendId(Kvasir::Diagnostic::Text& t,
                                unsigned long long        id) {
            t << ".";
            t.dec(id);
        }

        template<typename Rule, typename Tag, unsigned long long First, unsigned long long... Ids>
        struct DescribeExtra<Rule, Detail::ResourceT<Tag, First, Ids...>> {
            static constexpr void into(Kvasir::Diagnostic::Text& t) {
                t << ": ";
                if constexpr(requires { Tag::describeAs; }) {
                    t << std::string_view{Tag::describeAs};
                } else {
                    Kvasir::Diagnostic::appendType<Tag>(t);
                }
                t << " ";
                // describeHex: the first id is an address ("peripheral block at 0x42000800")
                if constexpr(KVASIR_CONFIG_OR(Tag, describeHex, false)) {
                    t.hex(static_cast<std::uint32_t>(First));
                } else {
                    t.dec(First);
                }
                (appendId(t, Ids), ...);
            }
        };

        template<typename Rule, typename T, T N>
        struct DescribeExtra<Rule, std::integral_constant<T, N>> {
            static constexpr void into(Kvasir::Diagnostic::Text& t) {
                constexpr auto kind = numberKindOf<Rule>;
                t << (kind == NumberKind::interrupt ? ": interrupt "
                      : kind == NumberKind::core    ? ": core "
                                                    : ": ");
                t.sdec(static_cast<std::int64_t>(N));
                if constexpr(kind == NumberKind::interrupt) {
                    constexpr std::string_view name = interruptName<Rule>(static_cast<int>(N));
                    if constexpr(!name.empty()) { t << " (" << name << ")"; }
                }
            }
        };

        // The report's text, built only when it fires (Kvasir::Diagnostic::Check): the rule, what
        // it is about, the peripheral, and the tag's own note.
        template<typename Rule, typename Peripheral, typename Extra>
        struct ReportText {
            consteval Kvasir::Diagnostic::Text operator()() const {
                Kvasir::Diagnostic::Text t;
                t << Rule::message;
                DescribeExtra<Rule, Extra>::into(t);
                t << " - ";
                Kvasir::Diagnostic::appendType<Peripheral>(t);
                constexpr std::string_view tag = tagMessage<typename TagOf<Extra>::type>;
                if constexpr(!tag.empty()) { t << " (" << tag << ")"; }
                return t;
            }
        };

        // Instantiated only for an offender: the assert always fires, its message names the
        // peripheral and what the rule is about, and the trail says the same. `Extra` carries
        // what the rule is about beyond the peripheral: the resource, or the core it should be
        // on, or an interrupt index. Each offender is its own report: a resource two
        // peripherals provide is two errors, one per provider.
        template<typename Rule, typename Peripheral, typename Extra = void>
        struct Report {
            static constexpr bool fires = MPL::AlwaysFalse<Report>::value;
            KVASIR_STATIC_ASSERT(fires,
                                 (ReportText<Rule,
                                             Peripheral,
                                             Extra>));
        };

        template<typename Rule, typename Offenders>
        struct ReportAll;

        template<typename Rule, typename... Ps, typename... Rs>
        struct ReportAll<Rule, brigand::list<Pair<Ps, Rs>...>> {
            // sizeof completes each Report, which is what fires it; no offenders, no reports
            static constexpr bool value = ((sizeof(Report<Rule, Ps, Rs>) > 0) && ...);
        };

        // ---- the offenders of each rule ---------------------------------------------------
        // A predicate is `Pred<Check>::template value<N, P, R>`: list index, peripheral,
        // one of its claims (or provides).
        namespace Detail {
            using namespace ::Kvasir::Startup::Detail;

            template<typename Pred, std::size_t N, typename P, typename Rs>
            struct PairsIf;

            template<typename Pred, std::size_t N, typename P, typename... Rs>
            struct PairsIf<Pred, N, P, brigand::list<Rs...>> {
                using type = brigand::flatten<
                  brigand::list<std::conditional_t<Pred::template value<N, P, Rs>,
                                                   brigand::list<Pair<P, Rs>>,
                                                   brigand::list<>>...>>;
            };

            // Of<N, P>::type: what entry P of list N is checked for (its claims, its provides).
            template<typename Pred,
                     template<std::size_t, typename> class Of,
                     std::size_t N,
                     typename List>
            struct OffendersInList;

            template<typename Pred,
                     template<std::size_t, typename> class Of,
                     std::size_t N,
                     typename... Ps>
            struct OffendersInList<Pred, Of, N, brigand::list<Ps...>> {
                using type = brigand::flatten<
                  brigand::list<typename PairsIf<Pred, N, Ps, typename Of<N, Ps>::type>::type...>>;
            };

            template<typename Pred,
                     template<std::size_t, typename> class Of,
                     typename Lists,
                     typename Seq>
            struct OffendersImpl;

            template<typename Pred,
                     template<std::size_t, typename> class Of,
                     typename... Ls,
                     std::size_t... Is>
            struct OffendersImpl<Pred, Of, brigand::list<Ls...>, std::index_sequence<Is...>> {
                using type = brigand::flatten<
                  brigand::list<typename OffendersInList<Pred, Of, Is, Ls>::type...>>;
            };

            template<typename Check>
            struct DoubleClaimPred {
                template<std::size_t N, typename P, typename R>
                static constexpr bool value
                  = !sharedClaim<typename R::tag> && MPL::count<R, typename Check::AllClaims> > 1;
            };

            template<typename Check>
            struct DoubleProvidePred {
                template<std::size_t N, typename P, typename R>
                static constexpr bool value = mergeIdentical<typename R::tag>
                                              ? !keyAgrees<R>(typename Check::AllProvides{})
                                              : countKeyIn<R>(typename Check::AllProvides{}) > 1;
            };

            template<typename Check, bool Listed>
            struct UnprovidedPred {
                template<std::size_t N, typename P, typename R>
                static constexpr bool value
                  = IsListedClaim<R>::value == Listed
                 && MPL::count<R, typename Check::AllProvides> == 0
                 && !(optionalProvider<typename R::tag>
                      && countTagIn<R>(typename Check::AllProvides{}) == 0);
            };

            template<typename Check, bool Listed>
            struct RemotePred {
                template<std::size_t N, typename P, typename R>
                static constexpr bool value
                  = IsListedClaim<R>::value == Listed && coreLocal<typename R::tag>
                 && MPL::count<R, typename Check::template ProvidesAt<N>> == 0
                 && MPL::count<R, typename Check::AllProvides> > 0;
            };

            // Affinity: the "resource" is the core the peripheral says it belongs to.
            template<typename P>
            struct CoreOf {
                using type = brigand::list<std::integral_constant<int, startupCoreOf<P>>>;
            };

            template<std::size_t N, typename P>
            using CoreOfAt = CoreOf<P>;

            template<std::size_t N, typename P>
            using ClaimsAt = AllClaimsOf<P>;

            // What P provides in list N: its own provides and the implicit Listed* ones, so the
            // double-provide walk covers every provide the sets hold.
            template<std::size_t N, typename P>
            struct ProvidesWithListedAt {
                using type
                  = brigand::append<typename ProvidesOf<P>::type,
                                    brigand::list<Listed<P>, ListedOn<P, N>, ListedHere<P>>>;
            };

            struct AffinityPred {
                template<std::size_t N, typename P, typename C>
                static constexpr bool value = C::value >= 0 && C::value != static_cast<int>(N);
            };
        }   // namespace Detail

        template<typename Check, typename Pred, template<std::size_t, typename> class Of>
        using Offenders = typename Detail::OffendersImpl<
          Pred,
          Of,
          typename Check::Lists,
          std::make_index_sequence<brigand::size<typename Check::Lists>::value>>::type;

        // The rules, with their messages.
        struct DoubleClaim {
            static constexpr std::string_view message
              = "a hardware resource is claimed by two peripherals: give each its own";
        };

        struct DoubleProvide {
            static constexpr std::string_view message
              = "a hardware resource is provided (configured, owned) by two peripherals";
        };

        struct Unprovided {
            static constexpr std::string_view message
              = "a peripheral claims a hardware resource nothing in any Startup list provides";
        };

        struct Remote {
            static constexpr std::string_view message
              = "a peripheral claims a hardware resource the other core's Startup list "
                "provides: the two belong in the same core's list";
        };

        struct Affinity {
            static constexpr auto             kind = NumberKind::core;
            static constexpr std::string_view message
              = "a peripheral that belongs to one core (its startupCore) is listed for the "
                "other";
        };

        struct Dependency {
            static constexpr std::string_view message
              = "a peripheral needs another one listed (on core N, or in its own list) that "
                "is not";
        };

        template<typename Check>
        using DoubleClaimOffenders
          = Offenders<Check, Detail::DoubleClaimPred<Check>, Detail::ClaimsAt>;
        template<typename Check>
        using DoubleProvideOffenders
          = Offenders<Check, Detail::DoubleProvidePred<Check>, Detail::ProvidesWithListedAt>;
        template<typename Check>
        using UnprovidedOffenders
          = Offenders<Check, Detail::UnprovidedPred<Check, false>, Detail::ClaimsAt>;
        template<typename Check>
        using RemoteOffenders
          = Offenders<Check, Detail::RemotePred<Check, false>, Detail::ClaimsAt>;
        template<typename Check>
        using UnlistedOffenders
          = Offenders<Check, Detail::UnprovidedPred<Check, true>, Detail::ClaimsAt>;
        template<typename Check>
        using RemoteListedOffenders
          = Offenders<Check, Detail::RemotePred<Check, true>, Detail::ClaimsAt>;
        template<typename Check>
        using DependencyOffenders
          = brigand::append<UnlistedOffenders<Check>, RemoteListedOffenders<Check>>;
        template<typename Check>
        using AffinityOffenders = Offenders<Check, Detail::AffinityPred, Detail::CoreOfAt>;

        // Every report for a ResourceCheck; true when none fired.
        template<typename Check>
        struct Diagnose {
            static constexpr bool value
              = ReportAll<Dependency, DependencyOffenders<Check>>::value
             && ReportAll<DoubleClaim, DoubleClaimOffenders<Check>>::value
             && ReportAll<DoubleProvide, DoubleProvideOffenders<Check>>::value
             && ReportAll<Unprovided, UnprovidedOffenders<Check>>::value
             && ReportAll<Remote, RemoteOffenders<Check>>::value
             && ReportAll<Affinity, AffinityOffenders<Check>>::value;
        };
    }   // namespace Diagnostics

    // The rules over one image: one peripheral list per core, core 0 first. Each rule is its
    // offender list (Diagnostics above); a rule holds when the list is empty, so a rule and
    // its report cannot disagree. One constant per rule, so a test can check each way.
    template<typename... CoreLists>
    struct ResourceCheck
      : Detail::ResourceSets<std::make_index_sequence<sizeof...(CoreLists)>, CoreLists...> {
        using Sets
          = Detail::ResourceSets<std::make_index_sequence<sizeof...(CoreLists)>, CoreLists...>;

        static constexpr bool noDoubleClaim
          = Diagnostics::none<Diagnostics::DoubleClaimOffenders<Sets>>;
        static constexpr bool noDoubleProvide
          = Diagnostics::none<Diagnostics::DoubleProvideOffenders<Sets>>;
        // every claim provided somewhere: the plain claims and the Listed* ones
        static constexpr bool claimsProvided
          = Diagnostics::none<Diagnostics::UnprovidedOffenders<Sets>>
         && Diagnostics::none<Diagnostics::UnlistedOffenders<Sets>>;
        // every core-local claim provided on its own core, the Listed* ones included
        static constexpr bool claimsLocal
          = Diagnostics::none<Diagnostics::RemoteOffenders<Sets>>
         && Diagnostics::none<Diagnostics::RemoteListedOffenders<Sets>>;
        static constexpr bool coreAffinityHonoured
          = Diagnostics::none<Diagnostics::AffinityOffenders<Sets>>;
        // rules 3 and 4 restricted to the Listed* claims: a peripheral needs another one listed
        static constexpr bool dependenciesListed
          = Diagnostics::none<Diagnostics::DependencyOffenders<Sets>>;

        static constexpr bool value = noDoubleClaim && noDoubleProvide && claimsProvided
                                   && claimsLocal && coreAffinityHonoured;
    };

    // A Startup entry for objects that live in main(): the union of the named types' Claims
    // and nothing else, so a driver constructed in main still has its pins (and whatever
    // else it claims) checked against the lists.
    //
    //   using Startup = Kvasir::Startup::Startup<ClockSettings, ..., Using<Display, Touch>>;
    template<typename... Ts>
    struct Using {
        static constexpr bool isStartupEntry = true;
        using Claims = brigand::flatten<brigand::list<typename Detail::ClaimsOf<Ts>::type...>>;
    };
}}   // namespace Kvasir::Startup
