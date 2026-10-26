// kvasir/Port/: the contract checks on a complete fake port (a fake chip and core declared before Check.hpp, as a
// firmware's chip and core are), Port::LateT naming the same type, the fallbacks losing to a real function, and the
// host values of the capability macros. port_must_fail/ has the cases where a piece is missing.
#include "kvasir/Common/Tags.hpp"
#include "kvasir/Port/Capabilities.hpp"
#include "kvasir/Port/Hooks.hpp"
#include "kvasir/Port/Interrupts.hpp"
#include "kvasir/Port/ResetKind.hpp"

#include <cstdint>
#include <type_traits>

// the fake chip and core
namespace Kvasir {
namespace Startup {
    template<typename... Ts>
    struct FirstInitStep<Tag::User, Ts...> {
        void operator()() const {}
    };

    template<typename... Ts>
    struct SecondaryCoreInit<Tag::User, Ts...> {
        static constexpr std::uint32_t cpacrEnable = 0;
    };

    namespace Core {
        inline void cleanCore() {}
    }   // namespace Core
}   // namespace Startup

namespace Port {
    // the chip's reboot (on a chip: Chip::reboot(), never returns); the fake counts and returns
    inline int reboots = 0;

    template<typename... Ts>
    struct Reboot<Tag::User, Ts...> {
        void operator()() const { ++reboots; }
    };
}   // namespace Port

namespace Nvic {
    template<>
    struct InterruptOffsetTraits<Kvasir::Tag::User> {
        static constexpr int begin = -14;
        static constexpr int end   = 32;
    };

    inline void clearAllEnablesAndPending() {}
}   // namespace Nvic

namespace PM {
    inline ResetKind resetKind() { return ResetKind::unknown; }
}   // namespace PM
}   // namespace Kvasir

#include "kvasir/Port/Check.hpp"
#include "test_harness.hpp"

namespace {
struct FakeStartup {};

using Clean = decltype(Kvasir::Startup::Core::cleanCore());
using Clear = decltype(Kvasir::Nvic::clearAllEnablesAndPending());

// Late names the same type, so a dependent spelling changes no mangled name
static_assert(std::is_same_v<Kvasir::Port::LateT<Kvasir::Startup::FirstInitStep<Kvasir::Tag::User>,
                                                 int>,
                             Kvasir::Startup::FirstInitStep<Kvasir::Tag::User>>);
// the reboot hook is found late through Port::LateT too
static_assert(std::is_same_v<Kvasir::Port::LateT<Kvasir::Port::Reboot<Kvasir::Tag::User>,
                                                 FakeStartup>,
                             Kvasir::Port::Reboot<Kvasir::Tag::User>>);
// the core's non-template functions win over the port's fallback templates
static_assert(std::is_same_v<Clean,
                             void>
              && std::is_same_v<Clear,
                                void>);
static_assert(std::is_same_v<decltype(Kvasir::PM::resetKind()),
                             Kvasir::ResetKind>);
// every piece there: the check holds
static_assert(Kvasir::Port::checkCore0<FakeStartup,
                                       Clean,
                                       Clear>());
static_assert(Kvasir::Port::ChipSecondaryCore<Kvasir::Startup::SecondaryCoreInit<Kvasir::Tag::User,
                                                                                 int>>);
static_assert(Kvasir::Port::ChipResetKind<decltype(Kvasir::PM::resetKind())>);

// and the concepts say no to what is not there
struct NoCpacr {};

static_assert(!Kvasir::Port::ChipSecondaryCore<NoCpacr>);
static_assert(!Kvasir::Port::CoreProvides<Kvasir::Port::Missing>);
static_assert(!Kvasir::Port::ChipResetKind<Kvasir::Port::Missing>);
// the host: no core, every capability 0
static_assert(KVASIR_CORE_ARMV6M == 0 && KVASIR_CORE_ARMV7EM == 0 && KVASIR_CORE_ARMV8M_MAIN == 0);
static_assert(KVASIR_CORE_HAS_CYCLE_COUNTER == 0 && KVASIR_CORE_HAS_DEBUGEN_READ == 0
              && KVASIR_CORE_HAS_STACK_LIMITS == 0 && KVASIR_CORE_HAS_FAULT_STATUS == 0);
}   // namespace

int main() {
    Kvasir::Test::test("portContract");
    Kvasir::Startup::FirstInitStep<Kvasir::Tag::User>{}();
    CHECK_EQ(static_cast<int>(Kvasir::PM::resetKind()),
             static_cast<int>(Kvasir::ResetKind::unknown));
    Kvasir::Port::LateT<Kvasir::Port::Reboot<Kvasir::Tag::User>, FakeStartup>{}();
    CHECK_EQ(Kvasir::Port::reboots, 1);
    return Kvasir::Test::report();
}
