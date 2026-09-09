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
// refuses to build unless:
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

#include "kvasir/Mpl/Types.hpp"

#include <array>
#include <cstddef>
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
    // I>) a literal write turns on. The chip layer specialises this for the NVIC's ISER
    // registers and SysTick's TICKINT; everything else enables nothing. Actions come from
    // `initStepInterruptConfig` and `initStepPeripheryEnable`, and Startup requires an Isr
    // in the same list for every index found (ListRules::EnabledLinesHandled).
    template<typename Action>
    struct InterruptOfAction {
        using type = brigand::list<>;
    };

    // The clock a core runs on, named here because the chip-agnostic core layer (SysTick)
    // claims it; the chip's clock settings provide it next to their own clocks. Optional,
    // so an application that does not declare its clocks stays unchecked rather than
    // broken.
    struct ProcessorClockTag {
        static constexpr bool sharedClaim      = true;
        static constexpr bool coreLocal        = false;
        static constexpr bool optionalProvider = true;
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
        constexpr bool sharedClaim = [] {
            if constexpr(requires { Tag::sharedClaim; }) {
                return static_cast<bool>(Tag::sharedClaim);
            } else {
                return false;
            }
        }();

        template<typename Tag>
        constexpr bool coreLocal = [] {
            if constexpr(requires { Tag::coreLocal; }) {
                return static_cast<bool>(Tag::coreLocal);
            } else {
                return true;
            }
        }();

        template<typename Tag>
        constexpr bool mergeIdentical = [] {
            if constexpr(requires { Tag::mergeIdentical; }) {
                return static_cast<bool>(Tag::mergeIdentical);
            } else {
                return false;
            }
        }();

        // How many leading ids form the key. One by default; a GPIO is (port, pin).
        template<typename Tag>
        constexpr std::size_t keyArity = [] {
            if constexpr(requires { Tag::keyArity; }) {
                return static_cast<std::size_t>(Tag::keyArity);
            } else {
                return std::size_t{1};
            }
        }();

        template<typename Tag>
        constexpr bool optionalProvider = [] {
            if constexpr(requires { Tag::optionalProvider; }) {
                return static_cast<bool>(Tag::optionalProvider);
            } else {
                return false;
            }
        }();

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
            for(std::size_t i = 0; i < K; ++i) {
                if(i >= NA || i >= NB) { return i >= NA && i >= NB; }
                if(a[i] != b[i]) { return false; }
            }
            return true;
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
        // initStepPinConfig is one action or a list of actions and lists; the same shapes
        // StartUp.hpp's Listify accepts.
        template<typename T>
        struct AsList {
            using type = brigand::list<T>;
        };

        template<typename... Ts>
        struct AsList<brigand::list<Ts...>> {
            using type = brigand::list<Ts...>;
        };

        template<typename List>
        struct ResourcesOfActions;

        template<typename... As>
        struct ResourcesOfActions<brigand::list<As...>> {
            using type = brigand::flatten<brigand::list<typename ResourceOfAction<As>::type...>>;
        };

        // The actions of one named init phase, flattened; empty when the peripheral has none.
        template<typename T, typename Get>
        struct PhaseActions {
            using type = brigand::list<>;
        };

        template<typename T, typename Get>
            requires requires { Get::template get<T>(); }
        struct PhaseActions<T, Get> {
            using type = brigand::flatten<
              typename AsList<std::remove_cvref_t<decltype(Get::template get<T>())>>::type>;
        };

        struct GetPowerClockEnable {
            template<typename T>
            static constexpr auto get() -> decltype(T::powerClockEnable) {
                return T::powerClockEnable;
            }
        };

        struct GetPinConfig {
            template<typename T>
            static constexpr auto get() -> decltype(T::initStepPinConfig) {
                return T::initStepPinConfig;
            }
        };

        struct GetPeripheryConfig {
            template<typename T>
            static constexpr auto get() -> decltype(T::initStepPeripheryConfig) {
                return T::initStepPeripheryConfig;
            }
        };

        struct GetInterruptConfig {
            template<typename T>
            static constexpr auto get() -> decltype(T::initStepInterruptConfig) {
                return T::initStepInterruptConfig;
            }
        };

        struct GetPeripheryEnable {
            template<typename T>
            static constexpr auto get() -> decltype(T::initStepPeripheryEnable) {
                return T::initStepPeripheryEnable;
            }
        };

        // Provided by doing: pins from initStepPinConfig, reset releases from
        // powerClockEnable (both through ResourceOfAction).
        template<typename T>
        struct DerivedProvides {
            using Actions = brigand::append<typename PhaseActions<T, GetPowerClockEnable>::type,
                                            typename PhaseActions<T, GetPinConfig>::type>;
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
            using Actions = brigand::append<typename PhaseActions<T, GetPinConfig>::type,
                                            typename PhaseActions<T, GetPeripheryConfig>::type,
                                            typename PhaseActions<T, GetInterruptConfig>::type,
                                            typename PhaseActions<T, GetPeripheryEnable>::type>;
            using type    = typename ClaimsOfActions<Actions>::type;
        };

        // ---- list helpers --------------------------------------------------------------
        template<typename R,
                 typename... Ts>
        constexpr std::size_t countIn(brigand::list<Ts...>) {
            return (std::size_t{0} + ... + std::size_t{std::is_same_v<R, Ts>});
        }

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

        // The list without repeats, first occurrence kept. A peripheral that writes one
        // pin's registers several times in its own init list provides that pin once.
        template<typename Out, typename In>
        struct UniqueImpl;

        template<typename... Os>
        struct UniqueImpl<brigand::list<Os...>, brigand::list<>> {
            using type = brigand::list<Os...>;
        };

        template<typename... Os, typename T, typename... Ts>
        struct UniqueImpl<brigand::list<Os...>, brigand::list<T, Ts...>>
          : UniqueImpl<std::conditional_t<(std::is_same_v<T, Os> || ...),
                                          brigand::list<Os...>,
                                          brigand::list<Os..., T>>,
                       brigand::list<Ts...>> {};

        template<typename T, typename... Ts>
        struct UniqueImpl<brigand::list<>, brigand::list<T, Ts...>>
          : UniqueImpl<brigand::list<T>, brigand::list<Ts...>> {};

        template<typename List>
        using Unique = typename UniqueImpl<brigand::list<>, List>::type;

        template<typename T>
        struct ProvidesOf {
            using type = Unique<brigand::append<typename ExplicitProvides<T>::type,
                                                typename DerivedProvides<T>::type>>;
        };

        // A peripheral's claims: what it says, as said, plus what its init steps imply (a
        // block written several times is claimed once).
        template<typename T>
        struct AllClaimsOf {
            using type = brigand::append<typename ClaimsOf<T>::type,
                                         Unique<typename DerivedClaims<T>::type>>;
        };

        // ---- the rules -----------------------------------------------------------------
        template<typename Claims>
        struct NoDoubleClaim;

        template<typename... Rs>
        struct NoDoubleClaim<brigand::list<Rs...>>
          : std::bool_constant<(
              (sharedClaim<typename Rs::tag> || countIn<Rs>(brigand::list<Rs...>{}) == 1) && ...)> {
        };

        template<typename Provides>
        struct NoDoubleProvide;

        template<typename... Rs>
        struct NoDoubleProvide<brigand::list<Rs...>>
          : std::bool_constant<((mergeIdentical<typename Rs::tag>
                                   ? keyAgrees<Rs>(brigand::list<Rs...>{})
                                   : countKeyIn<Rs>(brigand::list<Rs...>{}) == 1)
                                && ...)> {};

        // Every claim appears among the provides, exactly (payload included). A claim of an
        // optionalProvider tag is only held to that once anything provides the tag.
        template<typename Claims, typename Provides>
        struct ClaimsProvided;

        template<typename... Rs, typename Provides>
        struct ClaimsProvided<brigand::list<Rs...>, Provides>
          : std::bool_constant<(
              (countIn<Rs>(Provides{}) >= 1
               || (optionalProvider<typename Rs::tag> && countTagIn<Rs>(Provides{}) == 0))
              && ...)> {};

        // Every core-local claim whose provider exists anywhere has one on this core. A claim
        // with no provider at all is ClaimsProvided's business, not this one's.
        template<typename Claims, typename Here, typename Anywhere>
        struct ClaimsLocal;

        template<typename... Rs, typename Here, typename Anywhere>
        struct ClaimsLocal<brigand::list<Rs...>, Here, Anywhere>
          : std::bool_constant<((!coreLocal<typename Rs::tag> || countIn<Rs>(Here{}) >= 1
                                 || countIn<Rs>(Anywhere{}) == 0)
                                && ...)> {};

        // A peripheral with `static constexpr unsigned startupCore = N;` belongs in list N.
        template<typename T>
        constexpr int startupCoreOf = [] {
            if constexpr(requires { T::startupCore; }) {
                return static_cast<int>(T::startupCore);
            } else {
                return -1;
            }
        }();

        template<std::size_t Core, typename List>
        struct AffinityHonoured;

        template<std::size_t Core, typename... Ps>
        struct AffinityHonoured<Core, brigand::list<Ps...>>
          : std::bool_constant<(
              (startupCoreOf<Ps> < 0 || startupCoreOf<Ps> == static_cast<int>(Core)) && ...)> {};

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

        template<bool Keep, typename Out, typename In>
        struct SplitListedImpl;

        template<bool Keep, typename... Os>
        struct SplitListedImpl<Keep, brigand::list<Os...>, brigand::list<>> {
            using type = brigand::list<Os...>;
        };

        template<bool Keep, typename... Os, typename T, typename... Ts>
        struct SplitListedImpl<Keep, brigand::list<Os...>, brigand::list<T, Ts...>>
          : SplitListedImpl<Keep,
                            std::conditional_t<(IsListedClaim<T>::value == Keep),
                                               brigand::list<Os..., T>,
                                               brigand::list<Os...>>,
                            brigand::list<Ts...>> {};

        // The Listed* resources of a list, and the list without them.
        template<typename List>
        using KeepListed = typename SplitListedImpl<true, brigand::list<>, List>::type;

        template<typename List>
        using DropListed = typename SplitListedImpl<false, brigand::list<>, List>::type;

        template<typename Lists, typename Seq>
        struct AffinityOverLists;

        template<typename... Lists, std::size_t... Is>
        struct AffinityOverLists<brigand::list<Lists...>, std::index_sequence<Is...>>
          : std::bool_constant<(AffinityHonoured<Is, Lists>::value && ...)> {};

        template<typename Seq, typename... CoreLists>
        struct ResourceCheckImpl;

        template<std::size_t... Is, typename... CoreLists>
        struct ResourceCheckImpl<std::index_sequence<Is...>, CoreLists...> {
            using Lists = brigand::list<CoreLists...>;
            using AllProvides
              = brigand::flatten<brigand::list<typename ProvidesInAt<Is, CoreLists>::type...>>;
            using AllClaims
              = brigand::flatten<brigand::list<typename ClaimsIn<CoreLists>::type...>>;

            template<std::size_t N>
            using ProvidesAt = typename ProvidesInAt<N, brigand::at_c<Lists, N>>::type;

            static constexpr bool noDoubleClaim   = NoDoubleClaim<AllClaims>::value;
            static constexpr bool noDoubleProvide = NoDoubleProvide<AllProvides>::value;
            static constexpr bool claimsProvided  = ClaimsProvided<AllClaims, AllProvides>::value;
            static constexpr bool claimsLocal
              = (ClaimsLocal<typename ClaimsIn<CoreLists>::type,
                             typename ProvidesInAt<Is, CoreLists>::type,
                             AllProvides>::value
                 && ...);
            static constexpr bool coreAffinityHonoured
              = AffinityOverLists<brigand::list<CoreLists...>, std::index_sequence<Is...>>::value;

            // Rules 3 and 4 restricted to the Listed* claims: a subset of the above, so the
            // failure can be reported in those terms (a peripheral needs another one
            // listed) before the generic message would have to guess.
            static constexpr bool dependenciesListed
              = ClaimsProvided<KeepListed<AllClaims>, AllProvides>::value
             && (ClaimsLocal<KeepListed<typename ClaimsIn<CoreLists>::type>,
                             typename ProvidesInAt<Is, CoreLists>::type,
                             AllProvides>::value
                 && ...);

            static constexpr bool value = noDoubleClaim && noDoubleProvide && claimsProvided
                                       && claimsLocal && coreAffinityHonoured;
        };
    }   // namespace Detail

    // The rules over one image: one peripheral list per core, core 0 first. Each rule is
    // its own constant so Startup can name the one that failed and a test can check each
    // way.
    template<typename... CoreLists>
    struct ResourceCheck
      : Detail::ResourceCheckImpl<std::make_index_sequence<sizeof...(CoreLists)>, CoreLists...> {};

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

        template<typename Tag>
        constexpr std::string_view tagMessage = [] {
            if constexpr(requires { Tag::message; }) {
                return std::string_view{Tag::message};
            } else {
                return std::string_view{};
            }
        }();

        template<typename R>
        struct TagOf {
            using type = void;
        };

        template<typename Tag, unsigned long long... Ids>
        struct TagOf<Detail::ResourceT<Tag, Ids...>> {
            using type = Tag;
        };

        // The rule's text, then the tag's own if it has one: what static_assert prints.
        template<typename Rule, typename Resource>
        struct Message {
            static constexpr std::string_view rule = Rule::message;
            static constexpr std::string_view tag  = tagMessage<typename TagOf<Resource>::type>;
            static constexpr std::size_t size_   = rule.size() + (tag.empty() ? 0 : 2 + tag.size());
            static constexpr auto        storage = [] {
                std::array<char, size_ + 1> out{};
                std::size_t                 i = 0;
                for(char const c : rule) { out[i++] = c; }
                if(!tag.empty()) {
                    out[i++] = ':';
                    out[i++] = ' ';
                    for(char const c : tag) { out[i++] = c; }
                }
                return out;
            }();

            constexpr std::size_t size() const { return size_; }

            constexpr char const* data() const { return storage.data(); }
        };

        template<typename>
        struct AlwaysFalse : std::false_type {};

        // Instantiated only for an offender: the assert always fires, and the trail says
        // for whom. `Extra` carries what the rule is about beyond the peripheral: the
        // resource, or the core it should be on, or an interrupt index.
        template<typename Rule, typename Peripheral, typename Extra = void>
        struct Report {
            static_assert(AlwaysFalse<Report>::value,
                          Message<Rule,
                                  Extra>{});
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

            template<typename Pred, template<typename> class Of, std::size_t N, typename List>
            struct OffendersInList;

            template<typename Pred, template<typename> class Of, std::size_t N, typename... Ps>
            struct OffendersInList<Pred, Of, N, brigand::list<Ps...>> {
                using type = brigand::flatten<
                  brigand::list<typename PairsIf<Pred, N, Ps, typename Of<Ps>::type>::type...>>;
            };

            template<typename Pred, template<typename> class Of, typename Lists, typename Seq>
            struct OffendersImpl;

            template<typename Pred, template<typename> class Of, typename... Ls, std::size_t... Is>
            struct OffendersImpl<Pred, Of, brigand::list<Ls...>, std::index_sequence<Is...>> {
                using type = brigand::flatten<
                  brigand::list<typename OffendersInList<Pred, Of, Is, Ls>::type...>>;
            };

            template<typename Check>
            struct DoubleClaimPred {
                template<std::size_t N, typename P, typename R>
                static constexpr bool value
                  = !sharedClaim<typename R::tag> && countIn<R>(typename Check::AllClaims{}) > 1;
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
                 && countIn<R>(typename Check::AllProvides{}) == 0
                 && !(optionalProvider<typename R::tag>
                      && countTagIn<R>(typename Check::AllProvides{}) == 0);
            };

            template<typename Check, bool Listed>
            struct RemotePred {
                template<std::size_t N, typename P, typename R>
                static constexpr bool value
                  = IsListedClaim<R>::value == Listed && coreLocal<typename R::tag>
                 && countIn<R>(typename Check::template ProvidesAt<N>{}) == 0
                 && countIn<R>(typename Check::AllProvides{}) > 0;
            };

            // Affinity: the "resource" is the core the peripheral says it belongs to.
            template<typename P>
            struct CoreOf {
                using type = brigand::list<std::integral_constant<int, startupCoreOf<P>>>;
            };

            struct AffinityPred {
                template<std::size_t N, typename P, typename C>
                static constexpr bool value = C::value >= 0 && C::value != static_cast<int>(N);
            };
        }   // namespace Detail

        template<typename Check, typename Pred, template<typename> class Of>
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
        using DoubleClaimOffenders = Offenders<Check,
                                               Detail::DoubleClaimPred<Check>,
                                               ::Kvasir::Startup::Detail::AllClaimsOf>;
        template<typename Check>
        using DoubleProvideOffenders = Offenders<Check,
                                                 Detail::DoubleProvidePred<Check>,
                                                 ::Kvasir::Startup::Detail::ProvidesOf>;
        template<typename Check>
        using UnprovidedOffenders = Offenders<Check,
                                              Detail::UnprovidedPred<Check, false>,
                                              ::Kvasir::Startup::Detail::AllClaimsOf>;
        template<typename Check>
        using RemoteOffenders = Offenders<Check,
                                          Detail::RemotePred<Check, false>,
                                          ::Kvasir::Startup::Detail::AllClaimsOf>;
        template<typename Check>
        using DependencyOffenders
          = brigand::append<Offenders<Check,
                                      Detail::UnprovidedPred<Check, true>,
                                      ::Kvasir::Startup::Detail::AllClaimsOf>,
                            Offenders<Check,
                                      Detail::RemotePred<Check, true>,
                                      ::Kvasir::Startup::Detail::AllClaimsOf>>;
        template<typename Check>
        using AffinityOffenders = Offenders<Check, Detail::AffinityPred, Detail::CoreOf>;

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
