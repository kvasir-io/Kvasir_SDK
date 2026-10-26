// The default fatal end (Runtime/Panic.hpp): with the chip's Port::Reboot there, an image without a Reaction of its own
// records the panic and reboots - debugger or not; the nested panic (Detail::stop) reboots without a record; the
// decision table; the dispatch stops the other core (Port::StopOtherCore); the default fault reaction raises
// "fault". The chip's reboot is a stand-in that counts and leaves by longjmp (the harness's catch is not installed:
// the default handler runs).
#include "kvasir/Runtime/Panic.hpp"

#include "harness/PanicCatcher.hpp"
#include "kvasir/Runtime/FaultReaction.hpp"
#include "test_harness.hpp"

#include <csetjmp>
#include <type_traits>

namespace {
int     reboots = 0;
jmp_buf rebootBack;
}   // namespace

// the chip package's line (chip_rp_common/Reboot.hpp and the others), after the SDK header as in a firmware
template<typename... Ts>
struct Kvasir::Port::Reboot<Kvasir::Tag::User, Ts...> {
    [[noreturn]] void operator()() const {
        ++reboots;
        std::longjmp(rebootBack, 1);
    }
};

namespace { int stops = 0; }   // namespace

// a chip with a second core: the dispatch stops it right after the safe state
template<typename... Ts>
struct Kvasir::Port::StopOtherCore<Kvasir::Tag::User, Ts...> {
    void operator()() const { ++stops; }
};

namespace {
using Kvasir::Panic::Cause;
namespace D = Kvasir::Panic::Detail;

static_assert(D::canReboot<>,
              "a chip with Port::Reboot<Tag::User> can reboot");
static_assert(D::defaultEnd(true) == D::End::reset,
              "reset whenever the chip can, debugger or not");
static_assert(D::defaultEnd(false) == D::End::halt,
              "a chip without a reboot keeps the halt");
static_assert(std::is_same_v<D::ReactionOf<>,
                             Kvasir::Panic::OnPanic::Default>,
              "nothing specialised: the default");
static_assert(Kvasir::Panic::Handler<Kvasir::Panic::OnPanic::Wait>);
static_assert(Kvasir::Panic::Handler<Kvasir::Panic::OnPanic::Halt>);
static_assert(Kvasir::Panic::Handler<Kvasir::Panic::OnPanic::Reset<D::RebootOf<>>>);

template<typename F>
bool rebooted(F&& f) {
    reboots                        = 0;
    Kvasir::Panic::Detail::entered = false;
    if(setjmp(rebootBack) == 0) { f(); }
    return reboots == 1;
}

void defaultReboots() {
    Kvasir::Test::test("the default handler records the panic, then the chip's reboot");
#ifdef KVASIR_TEST_NO_LONGJMP
    return;
#endif
    Kvasir::Panic::lastPanic.clear();
    stops = 0;
    CHECK(rebooted([] { Kvasir::Panic::raiseAt(Cause::checkFailed, 0x1234, 7); }));
    CHECK(stops == 1);
    auto const r = Kvasir::Panic::takeLastPanic();
    CHECK(r && r->cause == static_cast<std::uint32_t>(Cause::checkFailed) && r->pc == 0x1234U
          && r->detail == 7U && r->count == 1U);
}

void nestedStops() {
    Kvasir::Test::test("a panic inside the handler: the reboot, without a second record");
#ifdef KVASIR_TEST_NO_LONGJMP
    return;
#endif
    Kvasir::Panic::lastPanic.clear();
    reboots                        = 0;
    Kvasir::Panic::Detail::entered = true;   // as inside the handler
    if(setjmp(rebootBack) == 0) { Kvasir::Panic::raise(Cause::user); }
    CHECK(reboots == 1);
    CHECK(!Kvasir::Panic::takeLastPanic().has_value());
    Kvasir::Panic::Detail::entered = false;
}

void faultAction() {
    Kvasir::Test::test(
      "Fault::OnFault::Panic, the default FaultReaction: a panic 'fault', detail = the exception "
      "number (0 here)");
#ifdef KVASIR_TEST_NO_LONGJMP
    return;
#endif
    Kvasir::Panic::lastPanic.clear();
    CHECK(rebooted([] { Kvasir::Fault::OnFault::Panic{}(); }));
    static_assert(std::is_same_v<Kvasir::Fault::FaultReaction<Kvasir::Tag::User>::type,
                                 Kvasir::Fault::OnFault::Panic>);
    auto const r = Kvasir::Panic::takeLastPanic();
    CHECK(r && r->cause == static_cast<std::uint32_t>(Cause::fault) && r->pc != 0U
          && r->detail == 0U);
}
}   // namespace

int main() {
    defaultReboots();
    nestedStops();
    faultAction();
    return Kvasir::Test::report();
}
