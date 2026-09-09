// Tests for kvasir/StartUp/Resources.hpp: the resource claims Startup checks across the
// two cores' peripheral lists.
//
// All of this is compile time only, so the test is a wall of static_asserts and main() just
// exists so ctest has something to run: if this file compiles, the tests passed. Every rule
// is checked both ways, since a check that can only say "yes" is no check.
#include "kvasir/StartUp/Resources.hpp"

#include "test_harness.hpp"

namespace {

using Kvasir::Startup::Resource;
using Kvasir::Startup::ResourceCheck;

template<typename... Ts>
using list = brigand::list<Ts...>;

namespace Dg = Kvasir::Startup::Diagnostics;

template<typename P, typename R>
using Pair = Dg::Pair<P, R>;

struct ChannelTag {};

struct LineTag {};

template<unsigned N>
using Ch = Resource<ChannelTag, N>;

template<unsigned N>
using Line = Resource<LineTag, N>;

// Stand-ins for what the chip layer builds: a DMA instance provides channels and a line, a
// driver claims channels. A peripheral with neither member is ignored.
template<typename... Rs>
struct Provider {
    using Provides = list<Rs...>;
};

template<typename... Rs>
struct Claimer {
    using Claims = list<Rs...>;
};

struct Plain {};

using Dma0 = Provider<Ch<0>, Ch<1>, Line<0>>;
using Dma1 = Provider<Ch<2>, Line<1>>;
using Spi  = Claimer<Ch<0>, Ch<1>>;
using Adc  = Claimer<Ch<2>>;

// ---- a good single-core image ----------------------------------------------------------
using Single = ResourceCheck<list<Plain, Dma0, Spi, Dma1, Adc>>;
static_assert(Single::noDoubleClaim);
static_assert(Single::noDoubleProvide);
static_assert(Single::claimsProvided);
static_assert(Single::claimsLocal);
static_assert(Single::value);

// ---- a good two-core image: each driver next to its instance ---------------------------
using Dual = ResourceCheck<list<Plain, Dma0, Spi>, list<Dma1, Adc>>;
static_assert(Dual::value);

// Order within a list does not matter, and a claimer may come before its provider.
static_assert(ResourceCheck<list<Adc,
                                 Spi,
                                 Dma1,
                                 Dma0>>::value);

// An empty image, and an image of peripherals that say nothing, pass.
static_assert(ResourceCheck<list<>>::value);
static_assert(ResourceCheck<list<Plain>,
                            list<>>::value);

// A provider nobody uses is fine (a channel kept for ad hoc use).
static_assert(ResourceCheck<list<Dma0,
                                 Dma1,
                                 Spi>>::value);

// ---- rule 1: no resource claimed twice --------------------------------------------------
using Spi2        = Claimer<Ch<0>>;   // a second driver on channel 0
using DoubleClaim = ResourceCheck<list<Dma0, Spi, Spi2>>;
static_assert(!DoubleClaim::noDoubleClaim);
static_assert(DoubleClaim::noDoubleProvide);
static_assert(DoubleClaim::claimsProvided);
static_assert(DoubleClaim::claimsLocal);
static_assert(!DoubleClaim::value);

// ... also across cores
static_assert(!ResourceCheck<list<Dma0,
                                  Spi>,
                             list<Spi2>>::noDoubleClaim);

// ... also within one peripheral
static_assert(!ResourceCheck<list<Dma0,
                                  Claimer<Ch<0>,
                                          Ch<0>>>>::noDoubleClaim);

// ---- rule 2: no resource provided twice -------------------------------------------------
using Legacy = Provider<Ch<0>, Ch<1>, Ch<2>, Line<0>, Line<1>, Line<2>, Line<3>>;

// two instances overlapping on a channel
using DoubleChannel = ResourceCheck<list<Dma0, Provider<Ch<1>, Line<1>>>>;
static_assert(!DoubleChannel::noDoubleProvide);
static_assert(DoubleChannel::noDoubleClaim);
static_assert(!DoubleChannel::value);

// two instances on one line, disjoint channels
static_assert(!ResourceCheck<list<Dma0,
                                  Provider<Ch<5>,
                                           Line<0>>>>::noDoubleProvide);

// a legacy instance owns every line, so any second instance collides with it
static_assert(!ResourceCheck<list<Legacy,
                                  Provider<Ch<5>,
                                           Line<3>>>>::noDoubleProvide);

// ... on the other core too: the controller is one piece of hardware
static_assert(!ResourceCheck<list<Legacy>,
                             list<Provider<Ch<5>,
                                           Line<3>>>>::noDoubleProvide);

// ---- rule 3: every claim has a provider -------------------------------------------------
using Unprovided = ResourceCheck<list<Dma0, Spi, Adc>>;   // Dma1 not listed
static_assert(!Unprovided::claimsProvided);
static_assert(Unprovided::noDoubleClaim);
static_assert(Unprovided::noDoubleProvide);
static_assert(!Unprovided::value);

// A claim with no provider anywhere is rule 3's failure alone: rule 4 stays quiet so the
// message names the actual problem.
static_assert(Unprovided::claimsLocal);

// ---- rule 4: the provider is on the claimer's core --------------------------------------
using Remote = ResourceCheck<list<Dma0, Spi, Adc>, list<Dma1>>;   // Adc on 0, its Dma on 1
static_assert(!Remote::claimsLocal);
static_assert(Remote::claimsProvided);
static_assert(Remote::noDoubleClaim);
static_assert(Remote::noDoubleProvide);
static_assert(!Remote::value);

// ... in the other direction
static_assert(!ResourceCheck<list<Dma0,
                                  Dma1,
                                  Adc>,
                             list<Spi>>::claimsLocal);

// ---- identities are the pair (tag, id) -------------------------------------------------
// The same number under another tag is another resource: channel 0 and line 0 coexist.
static_assert(ResourceCheck<list<Provider<Ch<0>>,
                                 Provider<Line<0>>>>::noDoubleProvide);
static_assert(std::is_same_v<Ch<3>,
                             Resource<ChannelTag,
                                      3>>);
static_assert(!std::is_same_v<Ch<3>,
                              Line<3>>);
// The id's spelling is not part of the identity: an int, an unsigned and an enumerator with
// one value name one resource.
enum class Channel : unsigned { ch3 = 3 };
static_assert(std::is_same_v<Resource<ChannelTag,
                                      3>,
                             Resource<ChannelTag,
                                      3u>>);
static_assert(std::is_same_v<Resource<ChannelTag,
                                      3>,
                             Resource<ChannelTag,
                                      Channel::ch3>>);
static_assert(std::is_same_v<Resource<ChannelTag,
                                      3,
                                      7>,
                             Resource<ChannelTag,
                                      3u,
                                      7u>>);

// ---- payload: a resource may carry more than its key ------------------------------------
// Identity is the whole type; the key is the tag and the first id.
struct SliceTag {
    static constexpr bool mergeIdentical = true;
};

template<unsigned N, unsigned Div, unsigned Top>
using Slice = Resource<SliceTag, N, Div, Top>;

// Two providers that agree on slice 0 are one provider; two that disagree are two.
static_assert(ResourceCheck<list<Provider<Slice<0,
                                                16,
                                                999>>,
                                 Provider<Slice<0,
                                                16,
                                                999>>>>::noDoubleProvide);
static_assert(!ResourceCheck<list<Provider<Slice<0,
                                                 16,
                                                 999>>,
                                  Provider<Slice<0,
                                                 32,
                                                 999>>>>::noDoubleProvide);
// ... and another slice is another resource whatever its payload
static_assert(ResourceCheck<list<Provider<Slice<0,
                                                16,
                                                999>>,
                                 Provider<Slice<1,
                                                32,
                                                5>>>>::noDoubleProvide);

// Without mergeIdentical an agreeing second provider is still a second provider.
struct StrictTag {};

static_assert(!ResourceCheck<list<Provider<Resource<StrictTag,
                                                    0,
                                                    1>>,
                                  Provider<Resource<StrictTag,
                                                    0,
                                                    1>>>>::noDoubleProvide);
// ... and a differing payload with the same key collides too: the key is the hardware.
static_assert(!ResourceCheck<list<Provider<Resource<StrictTag,
                                                    0,
                                                    1>>,
                                  Provider<Resource<StrictTag,
                                                    0,
                                                    2>>>>::noDoubleProvide);

// A claim must match the provider exactly, payload included.
static_assert(ResourceCheck<list<Provider<Resource<StrictTag,
                                                   0,
                                                   1>>,
                                 Claimer<Resource<StrictTag,
                                                  0,
                                                  1>>>>::claimsProvided);
static_assert(!ResourceCheck<list<Provider<Resource<StrictTag,
                                                    0,
                                                    1>>,
                                  Claimer<Resource<StrictTag,
                                                   0,
                                                   2>>>>::claimsProvided);

// ---- sharedClaim: several users of one thing --------------------------------------------
struct PinTag {
    static constexpr bool sharedClaim = true;
    static constexpr bool coreLocal   = false;
};

template<unsigned N>
using Pin = Resource<PinTag, N>;

using PinCfg = Provider<Pin<16>, Pin<22>>;
using Reset  = Claimer<Pin<16>>;
using Irq    = Claimer<Pin<22>>;
using Poll   = Claimer<Pin<22>>;   // a second reader of the same input

static_assert(ResourceCheck<list<PinCfg,
                                 Reset,
                                 Irq,
                                 Poll>>::value);
static_assert(ResourceCheck<list<PinCfg,
                                 Irq,
                                 Poll>>::noDoubleClaim);
// A shared claim still needs a provider ...
static_assert(!ResourceCheck<list<PinCfg,
                                  Claimer<Pin<9>>>>::claimsProvided);
// ... and two providers of one pin are still two.
static_assert(!ResourceCheck<list<PinCfg,
                                  Provider<Pin<16>>>>::noDoubleProvide);

// ---- coreLocal = false: the provider may be on either core ------------------------------
static_assert(ResourceCheck<list<PinCfg>,
                            list<Reset>>::claimsLocal);
static_assert(ResourceCheck<list<PinCfg>,
                            list<Reset>>::value);
// The DMA tags say nothing and so stay core-local (rule 4 above).
static_assert(!ResourceCheck<list<Dma0,
                                  Spi,
                                  Adc>,
                             list<Dma1>>::claimsLocal);

// ---- optionalProvider: checked once anything provides the tag ---------------------------
struct ClkTag {
    static constexpr bool sharedClaim      = true;
    static constexpr bool coreLocal        = false;
    static constexpr bool optionalProvider = true;
};

template<unsigned Hz>
using Clk = Resource<ClkTag, Hz>;

using Uart150 = Claimer<Clk<150'000'000>>;
using Uart200 = Claimer<Clk<200'000'000>>;
using Clocks  = Provider<Clk<200'000'000>>;

// Nobody provides a clock: every claim passes (an application that has not opted in).
static_assert(ResourceCheck<list<Uart150,
                                 Uart200>>::claimsProvided);
// Somebody does: the matching claim passes, the stale number fails.
static_assert(ResourceCheck<list<Clocks,
                                 Uart200>>::claimsProvided);
static_assert(!ResourceCheck<list<Clocks,
                                  Uart150>>::claimsProvided);
static_assert(!ResourceCheck<list<Clocks,
                                  Uart200,
                                  Uart150>>::claimsProvided);
// ... on either core
static_assert(ResourceCheck<list<Clocks>,
                            list<Uart200>>::value);

// Another optional tag being provided does not switch this one on.
struct OtherClkTag : ClkTag {};

static_assert(ResourceCheck<list<Provider<Resource<OtherClkTag,
                                                   1>>,
                                 Uart150>>::claimsProvided);

// ---- derived provides: what a peripheral does in initStepPinConfig ----------------------
// Stand-ins for register actions; the chip layer maps the ones that mean ownership.
template<unsigned Gpio>
struct CtrlWrite {};

template<unsigned Gpio>
struct PadWrite {};

}   // namespace

namespace Kvasir::Startup {
template<unsigned Gpio>
struct ResourceOfAction<CtrlWrite<Gpio>> {
    using type = brigand::list<Pin<Gpio>>;
};
}   // namespace Kvasir::Startup

namespace {

// One action, a list, a nested list: the shapes StartUp accepts.
struct OnePin {
    static constexpr auto initStepPinConfig = CtrlWrite<3>{};
};

struct SpiPins {
    static constexpr auto initStepPinConfig
      = list<PadWrite<16>, CtrlWrite<16>, list<PadWrite<18>, CtrlWrite<18>>, CtrlWrite<19>>{};
};

// A pin whose registers are written twice by one peripheral is provided once.
struct Twice {
    static constexpr auto initStepPinConfig = list<CtrlWrite<7>, PadWrite<7>, CtrlWrite<7>>{};
};

// Explicit and derived provides add up.
struct Both {
    using Provides                          = list<Ch<9>>;
    static constexpr auto initStepPinConfig = CtrlWrite<5>{};
};

// (AllProvides also carries the implicit Listed* resources of every entry, see below;
// DropListed leaves the explicit and derived ones.)
template<typename Check>
using Explicit = Kvasir::Startup::Detail::DropListed<typename Check::AllProvides>;

static_assert(std::is_same_v<Explicit<ResourceCheck<list<OnePin>>>,
                             list<Pin<3>>>);
static_assert(std::is_same_v<Explicit<ResourceCheck<list<SpiPins>>>,
                             list<Pin<16>,
                                  Pin<18>,
                                  Pin<19>>>);
static_assert(std::is_same_v<Explicit<ResourceCheck<list<Twice>>>,
                             list<Pin<7>>>);
static_assert(std::is_same_v<Explicit<ResourceCheck<list<Both>>>,
                             list<Ch<9>,
                                  Pin<5>>>);

// The point of it: a driver that configures pin 16 next to a PinConfig that also does.
static_assert(!ResourceCheck<list<PinCfg,
                                  SpiPins>>::noDoubleProvide);
static_assert(ResourceCheck<list<PinCfg,
                                 OnePin>>::noDoubleProvide);
// ... and a claim is satisfied by a derived provide.
static_assert(ResourceCheck<list<OnePin,
                                 Claimer<Pin<3>>>>::claimsProvided);
static_assert(ResourceCheck<list<SpiPins>,
                            list<Claimer<Pin<19>>>>::value);

// ---- derived claims and reset releases: what the other phases do ---------------------------
// Stand-ins: a write into block B, and the powerClockEnable write that releases block B.
template<unsigned B>
struct BlockWrite {};

template<unsigned B>
struct ReleaseWrite {};

struct BlockTag {
    static constexpr bool sharedClaim = true;
    static constexpr bool coreLocal   = false;
};

template<unsigned B>
using Block = Resource<BlockTag, B>;

}   // namespace

namespace Kvasir::Startup {
template<unsigned B>
struct ClaimOfAction<BlockWrite<B>> {
    using type = brigand::list<Block<B>>;
};

template<unsigned B>
struct ResourceOfAction<ReleaseWrite<B>> {
    using type = brigand::list<Block<B>>;
};
}   // namespace Kvasir::Startup

namespace {

// a driver that releases its block and then writes it, in several phases
struct Uart {
    static constexpr auto powerClockEnable        = ReleaseWrite<4>{};
    static constexpr auto initStepPeripheryConfig = list<BlockWrite<4>, BlockWrite<4>>{};
    static constexpr auto initStepPeripheryEnable = BlockWrite<4>{};
};

// a driver that writes a block nothing releases
struct Orphan {
    static constexpr auto initStepInterruptConfig = BlockWrite<5>{};
};

// pin configuration writes count too, and add to a declared claim
struct PinUser {
    using Claims                            = list<Ch<0>>;
    static constexpr auto initStepPinConfig = list<BlockWrite<6>, CtrlWrite<2>>{};
};

static_assert(std::is_same_v<Kvasir::Startup::Detail::AllClaimsOf<Uart>::type,
                             list<Block<4>>>);
static_assert(std::is_same_v<Kvasir::Startup::Detail::AllClaimsOf<PinUser>::type,
                             list<Ch<0>,
                                  Block<6>>>);
static_assert(std::is_same_v<Explicit<ResourceCheck<list<Uart>>>,
                             list<Block<4>>>);
// a powerClockEnable release is a provide, and satisfies the block's own writes ...
static_assert(ResourceCheck<list<Uart>>::claimsProvided);
// ... or another peripheral's, on either core
static_assert(ResourceCheck<list<Uart>,
                            list<Claimer<Block<4>>>>::value);
// a write into a block nothing released
static_assert(!ResourceCheck<list<Uart,
                                  Orphan>>::claimsProvided);
static_assert(std::is_same_v<Dg::UnprovidedOffenders<ResourceCheck<list<Uart,
                                                                        Orphan>>>,
                             list<Pair<Orphan,
                                       Block<5>>>>);

// ---- keyArity: the hardware is more than one id ------------------------------------------
struct PortPinTag {
    static constexpr unsigned keyArity = 2;
};

// (0, 5) and (0, 6) are two pins; (0, 5) twice is one pin twice.
static_assert(ResourceCheck<list<Provider<Resource<PortPinTag,
                                                   0,
                                                   5>>,
                                 Provider<Resource<PortPinTag,
                                                   0,
                                                   6>>>>::noDoubleProvide);
static_assert(!ResourceCheck<list<Provider<Resource<PortPinTag,
                                                    0,
                                                    5>>,
                                  Provider<Resource<PortPinTag,
                                                    0,
                                                    5>>>>::noDoubleProvide);
// A key shorter than its arity is its own key.
static_assert(!ResourceCheck<list<Provider<Resource<PortPinTag,
                                                    0>>,
                                  Provider<Resource<PortPinTag,
                                                    0>>>>::noDoubleProvide);
static_assert(ResourceCheck<list<Provider<Resource<PortPinTag,
                                                   0>>,
                                 Provider<Resource<PortPinTag,
                                                   0,
                                                   5>>>>::noDoubleProvide);

// ---- core affinity ----------------------------------------------------------------------
struct OnCore0 {
    static constexpr unsigned startupCore = 0;
};

struct OnCore1 {
    static constexpr unsigned startupCore = 1;
};

static_assert(ResourceCheck<list<OnCore0>,
                            list<OnCore1>>::coreAffinityHonoured);
static_assert(ResourceCheck<list<OnCore0,
                                 Plain>>::coreAffinityHonoured);
static_assert(!ResourceCheck<list<OnCore1>,
                             list<>>::coreAffinityHonoured);
static_assert(!ResourceCheck<list<>,
                             list<OnCore0>>::coreAffinityHonoured);
static_assert(!ResourceCheck<list<OnCore1>>::coreAffinityHonoured);
static_assert(!ResourceCheck<list<OnCore0>,
                             list<OnCore0>>::value);
static_assert(ResourceCheck<list<OnCore0>,
                            list<OnCore1>>::value);

// ---- dependencies between listed peripherals ---------------------------------------------
// Every entry implicitly provides Listed<T>, ListedOn<T, core> and ListedHere<T>; a
// peripheral that needs another one on a particular core claims one of them.
using Kvasir::Startup::Listed;
using Kvasir::Startup::ListedHere;
using Kvasir::Startup::ListedOn;

struct RefClock {};

struct TargetClock {};

using Sync    = Claimer<ListedOn<RefClock, 0>, ListedHere<TargetClock>>;   // ClockSync's shape
using Timeout = Claimer<ListedOn<RefClock, 0>>;                            // LaunchTimeout's
using Any     = Claimer<Listed<RefClock>>;

// the same type twice in one list is Startup's "listed twice" finding, not a double provide
static_assert(ResourceCheck<list<Plain,
                                 Plain>>::noDoubleProvide);

// the implicit provides of a one-entry list
static_assert(std::is_same_v<ResourceCheck<list<RefClock>>::AllProvides,
                             list<Listed<RefClock>,
                                  ListedOn<RefClock,
                                           0>,
                                  ListedHere<RefClock>>>);

// the good shape: the reference on core 0, the target next to the sync on core 1
using GoodSync = ResourceCheck<list<RefClock>, list<TargetClock, Sync, Timeout>>;
static_assert(GoodSync::dependenciesListed);
static_assert(GoodSync::value);

// the reference missing from core 0 (on core 1 instead): ListedOn<RefClock, 0> unprovided
using RefOnWrongCore = ResourceCheck<list<Plain>, list<RefClock, TargetClock, Sync>>;
static_assert(!RefOnWrongCore::dependenciesListed);
static_assert(!RefOnWrongCore::claimsProvided);

// the reference not listed at all
static_assert(!ResourceCheck<list<>,
                             list<TargetClock,
                                  Sync>>::dependenciesListed);

// the target on the other core: ListedHere is core-local, so rule 4 says no
using TargetRemote = ResourceCheck<list<RefClock, TargetClock>, list<Sync>>;
static_assert(!TargetRemote::dependenciesListed);
static_assert(!TargetRemote::claimsLocal);
static_assert(TargetRemote::claimsProvided);

// Listed<T> is satisfied by either core
static_assert(ResourceCheck<list<RefClock>,
                            list<Any>>::dependenciesListed);
static_assert(ResourceCheck<list<Any>,
                            list<RefClock>>::dependenciesListed);
static_assert(!ResourceCheck<list<Any>,
                             list<>>::dependenciesListed);

// dependenciesListed looks at the Listed* claims only: an unrelated missing provider is
// the generic rule's finding
using OtherMissing = ResourceCheck<list<RefClock, Adc>, list<>>;
static_assert(OtherMissing::dependenciesListed);
static_assert(!OtherMissing::claimsProvided);

// ---- Using<...>: claims of objects that live in main() ------------------------------------
using Kvasir::Startup::Using;

static_assert(std::is_same_v<Using<Reset,
                                   Irq>::Claims,
                             list<Pin<16>,
                                  Pin<22>>>);
static_assert(std::is_same_v<Using<Plain>::Claims,
                             list<>>);
static_assert(Using<Reset>::isStartupEntry);
static_assert(ResourceCheck<list<PinCfg,
                                 Using<Reset,
                                       Irq>>>::value);
static_assert(!ResourceCheck<list<PinCfg,
                                  Using<Claimer<Pin<9>>>>>::claimsProvided);

// ---- diagnostics: which peripheral, which resource ----------------------------------------

// a rule that holds has no offenders
static_assert(std::is_same_v<Dg::DoubleClaimOffenders<Single>,
                             list<>>);
static_assert(std::is_same_v<Dg::DoubleProvideOffenders<Single>,
                             list<>>);
static_assert(std::is_same_v<Dg::UnprovidedOffenders<Single>,
                             list<>>);
static_assert(std::is_same_v<Dg::RemoteOffenders<Dual>,
                             list<>>);
static_assert(std::is_same_v<Dg::AffinityOffenders<ResourceCheck<list<OnCore0>,
                                                                 list<OnCore1>>>,
                             list<>>);
static_assert(Dg::Diagnose<Single>::value);
static_assert(Dg::Diagnose<Dual>::value);

// both claimers of channel 0 are named (the second driver and the first)
static_assert(std::is_same_v<Dg::DoubleClaimOffenders<DoubleClaim>,
                             list<Pair<Spi,
                                       Ch<0>>,
                                  Pair<Spi2,
                                       Ch<0>>>>);
// both providers of channel 1
static_assert(std::is_same_v<Dg::DoubleProvideOffenders<DoubleChannel>,
                             list<Pair<Dma0,
                                       Ch<1>>,
                                  Pair<Provider<Ch<1>,
                                                Line<1>>,
                                       Ch<1>>>>);
// the claim with no provider
static_assert(std::is_same_v<Dg::UnprovidedOffenders<Unprovided>,
                             list<Pair<Adc,
                                       Ch<2>>>>);
// the claim whose provider is on the other core
static_assert(std::is_same_v<Dg::RemoteOffenders<Remote>,
                             list<Pair<Adc,
                                       Ch<2>>>>);
// the peripheral on the wrong core, with the core it wanted
static_assert(std::is_same_v<Dg::AffinityOffenders<ResourceCheck<list<OnCore1>,
                                                                 list<>>>,
                             list<Pair<OnCore1,
                                       std::integral_constant<int,
                                                              1>>>>);
// a listed-dependency claim is the Dependency rule's, not Unprovided's
static_assert(std::is_same_v<Dg::DependencyOffenders<RefOnWrongCore>,
                             list<Pair<Sync,
                                       ListedOn<RefClock,
                                                0>>>>);
static_assert(std::is_same_v<Dg::UnprovidedOffenders<RefOnWrongCore>,
                             list<>>);
static_assert(std::is_same_v<Dg::DependencyOffenders<TargetRemote>,
                             list<Pair<Sync,
                                       ListedHere<TargetClock>>>>);

// the message: the rule's text, then the tag's own if it has one
struct TalkativeTag {
    static constexpr char const* message = "one per group";
};

using TalkativeMsg = Dg::Message<Dg::DoubleProvide, Resource<TalkativeTag, 0>>;
using PlainMsg     = Dg::Message<Dg::DoubleProvide, Ch<0>>;
static_assert(std::string_view{TalkativeMsg{}.data(), TalkativeMsg{}.size()}
              == "a hardware resource is provided (configured, owned) by two peripherals: one "
                 "per group");
static_assert(std::string_view{PlainMsg{}.data(),
                               PlainMsg{}.size()}
              == Dg::DoubleProvide::message);

}   // namespace

int main() {
    // everything above is checked at compile time; reaching main means it all held
    Kvasir::Test::test("resources");
    return Kvasir::Test::report();
}
