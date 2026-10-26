#!/usr/bin/env python3
"""Kvasir's named compile errors: compile each snippet with -fsyntax-only, expect it to fail, and expect the register,
field and resource names in the compiler's output.

    test_diagnostics_must_fail.py --cxx <c++ compiler> --src <Kvasir_SDK/src> --tests <Kvasir_SDK/tests> [--flag=<flag>]...
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tooling"))
from must_fail import Case, run  # noqa: E402

HEAD = '''#include "test_registers.hpp"
#include "kvasir/StartUp/Resources.hpp"
#include "kvasir/Io/Claims.hpp"
#include "kvasir/StartUp/ListRules.hpp"
#include <array>
#include <string_view>
namespace R = Kvasir::Register;
// a key register that names itself, as svd_converter generates it (Mode's second argument)
struct TESTREG {
    using Addr = R::Address<0x40, 0, 0, std::uint32_t, R::Mode<{.mustSupply = 0xFFFF0000U}, TESTREG>>;
    static constexpr R::FieldLocation<Addr, R::maskFromRange(31, 16), R::ReadWriteAccess, std::uint32_t> key{};
    static constexpr R::FieldLocation<Addr, R::maskFromRange(7, 0), R::ReadWriteAccess, std::uint32_t> data{};
    static constexpr std::string_view fmt_string{"test::testreg(\\n\\"key\\": 0x{:x}\\n\\"data\\": 0x{:x})"};
    static constexpr auto field_names = std::to_array<std::string_view>({"key", "data"});
    using fields = R::Fields<decltype(key), decltype(data)>;
};
// a register whose read pops a FIFO
struct POPREG {
    using Addr = R::Address<0x44, 0, 0, std::uint32_t, R::Mode<{.readSideEffect = true}, POPREG>>;
    static constexpr R::FieldLocation<Addr, R::maskFromRange(7, 0), R::ReadWriteAccess, std::uint32_t> data{};
    static constexpr R::FieldLocation<Addr, R::maskFromRange(8, 8), R::ReadWriteAccess, std::uint32_t> last{};
    static constexpr std::string_view fmt_string{"test::popreg(\\n\\"data\\": 0x{:x}\\n\\"last\\": 0x{:x})"};
    static constexpr auto field_names = std::to_array<std::string_view>({"data", "last"});
    using fields = R::Fields<decltype(data), decltype(last)>;
};
// a write-only register with a key (no SVD has one yet): never read, so a write must name the key
struct WOKEYREG {
    using Addr
      = R::Address<0x50, 0, 0, std::uint32_t, R::Mode<{.mustSupply = 0xFF000000U, .writeOnlyRegister = true}, WOKEYREG>>;
    static constexpr R::FieldLocation<Addr, R::maskFromRange(31, 24), R::WriteOnlyAccess, std::uint32_t> key{};
    static constexpr R::FieldLocation<Addr, R::maskFromRange(0, 0), R::WriteOnlyAccess, std::uint32_t> go{};
    static constexpr std::string_view fmt_string{"test::wokeyreg(\\n\\"key\\": 0x{:x}\\n\\"go\\": 0x{:x})"};
    static constexpr auto field_names = std::to_array<std::string_view>({"key", "go"});
    using fields = R::Fields<decltype(key), decltype(go)>;
};
// a normal register named by NAMED_REGISTERS (phase 2)
struct CTRLREG {
    using Addr = R::Address<0x48, 0, 0, std::uint32_t, R::Mode<{}, CTRLREG>>;
    static constexpr R::FieldLocation<Addr, R::maskFromRange(7, 4), R::ReadWriteAccess, std::uint32_t> div{};
    static constexpr R::FieldLocation<Addr, R::maskFromRange(0, 0), R::ReadWriteAccess, std::uint32_t> en{};
    static constexpr std::string_view fmt_string{"test::ctrlreg(\\n\\"div\\": 0x{:x}\\n\\"en\\": 0x{:x})"};
    static constexpr auto field_names = std::to_array<std::string_view>({"div", "en"});
    using fields = R::Fields<decltype(div), decltype(en)>;
};
// enables and a one-to-set (self-clearing) trigger per machine, as PIO CTRL (Register::atomic takes set() of a trigger)
struct TRIGREG {
    using Addr = R::Address<0x54, 0xFFFFFFF0U, 0, std::uint32_t, R::Mode<{.writeBackActs = 0xF0U}, TRIGREG>>;
    using TriggerAccess
      = R::Access<R::AccessType::readWrite, R::ReadActionType::normal, R::ModifiedWriteValueType::oneToSet>;
    static constexpr R::FieldLocation<Addr, R::maskFromRange(3, 0), R::ReadWriteAccess, std::uint32_t> enable{};
    static constexpr R::FieldLocation<Addr, R::maskFromRange(7, 4), TriggerAccess, std::uint32_t> restart{};
    static constexpr std::string_view fmt_string{"test::trigreg(\\n\\"enable\\": 0x{:x}\\n\\"restart\\": 0x{:x})"};
    static constexpr auto field_names = std::to_array<std::string_view>({"enable", "restart"});
    using fields = R::Fields<decltype(enable), decltype(restart)>;
};
// and one without a name
struct PLAINREG {
    using Addr = R::Address<0x4C, 0, 0, std::uint32_t>;
    static constexpr R::FieldLocation<Addr, R::maskFromRange(7, 4), R::ReadWriteAccess, std::uint32_t> div{};
    static constexpr R::FieldLocation<Addr, R::maskFromRange(0, 0), R::ReadWriteAccess, std::uint32_t> en{};
};
'''

# the hook-shape rule as Startup's check block reports it (its offender list, WrongHookShapes), on a one-entry list:
# the offender, if any, is the entry
HOOK_ASSERT = """static constexpr bool hookShapesHold
  = std::is_same_v<Kvasir::Startup::ListRules::WrongHookShapes<brigand::list<{0}>>, brigand::list<>>;
KVASIR_STATIC_ASSERT(hookShapesHold, (Kvasir::Startup::ListRules::HookShapeText<{0}>));
"""

# the shared check block (StartUp/Checks.hpp) on fake lists: a fake chip, SecondaryCore and backend
CHECKS = """#include "kvasir/StartUp/Checks.hpp"
namespace C = Kvasir::Startup::Checks;
struct Traits { static constexpr int begin = -14; static constexpr int end = 32; };
template<typename... Ps> struct Core1 {};
template<typename T> struct IsCore1 : std::false_type {};
template<typename... Ps> struct IsCore1<Core1<Ps...>> : std::true_type {};
template<typename T> struct Core1List { using type = brigand::list<>; };
template<typename... Ps> struct Core1List<Core1<Ps...>> { using type = brigand::list<Ps...>; };
struct NoBackend;
using Ctx = C::Context<Traits, IsCore1, Core1List, NoBackend>;
template<int I> struct FakeIsr { using IType = std::integral_constant<int, I>; };
template<int I> struct WithIsr { using Isr = brigand::list<FakeIsr<I>>; };
struct Dup { static constexpr int initStepPinConfig = 0; };
struct Cfg { static constexpr int instance = 0; };
struct Clocks { static void coreClockInit() {} static void peripheryClockInit() {} };
template<C::Role R, int N, typename... Ps>
constexpr bool checked = C::CheckList<Ctx, R, N, void, brigand::list<Ps...>, brigand::list<Ps...>>::value;
template<typename... Ps>
constexpr bool image = C::ImageChecks<Ctx, void, Clocks, brigand::list<Ps...>, brigand::list<Ps...>>::value;
"""

# the boot report rules on fake lists: a context whose user backend is Log
BOOT = """struct Log { static void runtimeInit() {} };
struct Report { static constexpr bool isBootReport = true; static void runtimeInit() {} };
struct InMain { static constexpr bool isStartupEntry = true; static constexpr bool bootReportInMain = true; };
struct Guard { static constexpr bool isBootGuard = true; static void runtimeInit() {} };
struct Bundle { using StartupEntries = brigand::list<Guard, Report>; };
struct Service { static constexpr bool isHealthService = true; static void runtimeInit() {} };
struct Dog { static constexpr bool isWatchdog = true; static void runtimeInit() {} };
using CtxL = C::Context<Traits, IsCore1, Core1List, Log>;
template<C::Role R, int N, typename... Ps>
constexpr bool checkedL = C::CheckList<CtxL, R, N, void, Kvasir::Startup::Detail::Entries<Ps...>,
                                       Kvasir::Startup::Detail::Entries<Ps...>>::value;
"""

# the image's CRC engine set on fake lists (Kvasir::Crc::ImageEngines' stand-in: a hook whose `type` is the set): a
# set that claims hardware (a chip's: the sniffer's DMA channel) must be in one of the image's lists
CRCSET = """struct SnifferTag {};
struct ClaimingSet { using Provides = brigand::list<Kvasir::Startup::Resource<SnifferTag, 0>>; };
struct SoftwareSet {};
template<typename Set> struct Hook { using type = Set; };
template<typename Set>
using CtxC = C::Context<Traits, IsCore1, Core1List, NoBackend, Hook<Set>>;
template<typename Set, typename... Ps>
constexpr bool imageC = C::ImageChecks<CtxC<Set>, void, Clocks, brigand::list<Ps...>, brigand::list<Ps...>>::value;
"""

# (name, snippet, extra flags, substrings every compiler prints; None: a control that must compile)
ROWS = [
    # Register::atomic of a one-to-set trigger: set() only; a flag stays refused (AtomicFactories.hpp)
    ("atomic set of a one-to-set trigger and of an enable (a control: want None)",
     "void f() { apply(R::atomic(set(R::bit<1>(TRIGREG::restart)), clear(R::bit<2>(TRIGREG::enable)))); }\n", [],
     None),
    ("atomic write of 0 to a one-to-set trigger",
     "void f() { apply(R::atomic(write(R::bit<1>(TRIGREG::restart), R::value<std::uint32_t, 0>()))); }\n", [],
     ["Register::atomic on field(s) restart of test::trigreg (0x00000054)",
      "a one-to-set trigger: it takes set() only (a written 0 does nothing)"]),
    ("atomic clear of a one-to-set trigger (clear() refuses it first)",
     "void f() { apply(R::atomic(clear(R::bit<1>(TRIGREG::restart)))); }\n", [],
     ["Register::clear writes a 0, which field(s)", "test::trigreg (0x00000054)", "use Register::reset"]),
    ("atomic set of a write-one-to-clear flag",
     "void f() { apply(R::atomic(set(CtrlReg::flag))); }\n", [],
     ["Register::atomic on", "a one-to-clear/one-to-toggle or read-only field: use reset() for a flag"]),
    # fieldAt<Bit>(R{}): the generated field that holds a bit (Register.hpp)
    ("fieldAt of a bit in a field (a control: want None)",
     "static_assert(std::is_same_v<decltype(R::fieldAt<5>(TRIGREG{})), std::remove_cvref_t<decltype(TRIGREG::restart)>>);\n"
     "static_assert(std::is_same_v<R::FieldAt<TESTREG, 31>, std::remove_cvref_t<decltype(TESTREG::key)>>);\n", [], None),
    ("fieldAt of a reserved bit",
     "void f() { static_cast<void>(R::fieldAt<10>(TESTREG{})); }\n", [],
     ["fieldAt<10>: bit 10 of test::testreg (0x00000040) is in no field"]),
    ("fieldAt of a bit outside the register",
     "using F = R::FieldAt<TESTREG, 40>;\nF const* p = nullptr;\n", [],
     ["fieldAt<40>: bit 40 of test::testreg (0x00000040) is outside the register"]),
    ("key register, partial write",
     "void f() { apply(write(TESTREG::data, R::value<std::uint32_t, 5>())); }\n", [],
     ["write of test::testreg (0x00000040) field(s) data is a read-modify-write",
      "key field(s) key (0xFFFF0000)"]),
    ("side-effect register, partial write",
     "void f() { apply(write(POPREG::data, R::value<std::uint32_t, 5>())); }\n", [],
     ["write of test::popreg (0x00000044) field(s) data", "reading it has a side effect"]),
    ("write-only register with a key, a write without it",
     "void f() { apply(set(WOKEYREG::go)); }\n", [],
     ["write of test::wokeyreg (0x00000050) field(s) go does not name the key field(s) key (0xFF000000)",
      "the register is write-only"]),
    ("write-only register with a key, the key named (a control: want None)",
     "void f() { apply(write(WOKEYREG::key, R::value<std::uint32_t, 0x5A>()), set(WOKEYREG::go)); }\n", [], None),
    ("literal too wide, named register",
     "void f() { apply(write(CTRLREG::div, R::value<std::uint32_t, 20>())); }\n", [],
     ["literal 20 does not fit field(s) div of test::ctrlreg (0x00000048) (it holds 0..15)"]),
    ("literal too wide, unnamed register",
     "void f() { apply(write(PLAINREG::div, R::value<std::uint32_t, 20>())); }\n", [],
     ["literal 20 does not fit bits 7:4 of register 0x0000004C (it holds 0..15)"]),
    ("set on a wide field",
     "void f() { apply(set(CTRLREG::div)); }\n", [],
     ["Register::set only works on single bits, field(s) div of test::ctrlreg (0x00000048) is bits 7:4"]),
    # the factories' and the whole-register helpers' named checks (Register/Factories.hpp, Register.hpp)
    ("read of a write-only field",
     "void f() { apply(read(AccessTestReg::wo)); }\n", [],
     ["Access violation: bit 1 of register 0x00000080 is not marked as readable"]),
    ("runtime write of a read-only field",
     "void f(unsigned v) { apply(write(AccessTestReg::ro, Kvasir::Test::runtimeValue(v))); }\n", [
     ],
     ["Access violation: bit 0 of register 0x00000080 is not marked as writable"]),
    ("reset of a field that is not write-one-to-clear",
     "void f() { apply(reset(CTRLREG::en)); }\n", [],
     ["Register::reset writes ones to clear a flag: field(s) en of test::ctrlreg (0x00000048) is not "
      "write-one-to-clear"]),
    ("write of a compile-time value of another type",
     "void f() { apply(write(CTRLREG::div, R::value<std::uint8_t, 2>())); }\n", [],
     ["type mismatch: the compile time value written to field(s) div of test::ctrlreg (0x00000048) must have the "
      "field's type"]),
    # every std::integral_constant reaches the literal write() and its type check, never the runtime write through
    # integral_constant's conversion
    ("write of a std::integral_constant of another type",
     "void f() { apply(write(CTRLREG::div, std::integral_constant<int, 1>{})); }\n", [],
     ["type mismatch: the compile time value written to field(s) div of test::ctrlreg (0x00000048) must have the "
      "field's type"]),
    ("write of an unsigned std::integral_constant to a bool field",
     "struct BOOLREG { using Addr = R::Address<0x58, 0, 0, std::uint32_t>;\n"
     "  static constexpr R::FieldLocation<Addr, 1U, R::ReadWriteAccess, bool> on{}; };\n"
     "void f() { apply(write(BOOLREG::on, std::integral_constant<unsigned, 1>{})); }\n", [],
     ["type mismatch: the compile time value written to"]),
    ("write of a std::integral_constant of the field's type compiles (a control: want None)",
     "void f() { apply(write(CTRLREG::div, std::integral_constant<R::Detail::GetFieldTypeT<std::remove_cvref_t<decltype(CTRLREG::div)>>,"
     " 5>{})); }\n", [], None),
    ("write of an interrupt index as a value",
     "#include \"kvasir/Common/Interrupt.hpp\"\n"
     "void f() { apply(write(CTRLREG::div, Kvasir::Nvic::Index<3>{})); }\n", [],
     ["type mismatch: the compile time value written to field(s) div of test::ctrlreg (0x00000048) must have the "
      "field's type"]),
    ("write of a pin tag as a value",
     "#include \"kvasir/Io/Io.hpp\"\n"
     "void f() { apply(write(CTRLREG::div, Kvasir::Io::pin3)); }\n", [],
     ["type mismatch: the compile time value written to field(s) div of test::ctrlreg (0x00000048) must have the "
      "field's type"]),
    # Register's variadic factories take fields only since the pins and their factories live in Kvasir::Io
    ("a pin in Register's variadic set",
     "void f() { apply(R::set(CTRLREG::div, Kvasir::Io::PinLocation<0, 3>{})); }\n", [],
     # gcc quotes the name in typographic quotes, clang in straight ones
     ["no matching function for call to"]),
    ("clear of a write-only toggle field",
     "struct WoToggle { using Addr = R::Address<0x54, 0xFFFFFFFFU, 0, std::uint32_t>;\n"
     "  static constexpr R::FieldLocation<Addr, 1U, R::Access<R::AccessType::writeOnly, R::ReadActionType::normal,"
     " R::ModifiedWriteValueType::oneToToggle>, std::uint32_t> tgl{}; };\n"
     "void f() { apply(clear(WoToggle::tgl)); }\n", [],
     ["Register::clear on a write-only toggle field: bit 0 of register 0x00000054 cannot be read back"]),
    # a whole register's FULLREGISTER carries the register's access (svd_converter: write-only when no field can
    # be read, a field-less register's own <access>)
    ("readWhole of a write-only register",
     "unsigned f() { return R::readWhole(WholeWriteOnlyReg::FULLREGISTER); }\n", [],
     ["Access violation: bits 31:0 of register 0x000000E8 is not marked as readable"]),
    ("read() of a write-only register's FULLREGISTER",
     "unsigned f() { return get<0>(apply(read(WholeWriteOnlyReg::FULLREGISTER))); }\n", [],
     ["Access violation: bits 31:0 of register 0x000000E8 is not marked as readable"]),
    ("writeWhole of a read-only register",
     "void f() { R::writeWhole(WholeReadOnlyReg::FULLREGISTER, 1U); }\n", [],
     ["Access violation: bits 31:0 of register 0x000000EC is not marked as writable"]),
    ("readWhole of a field",
     "unsigned f() { return R::readWhole(CTRLREG::div); }\n", [],
     ["readWhole and writeWhole take the register's FULLREGISTER, not field(s) div of test::ctrlreg (0x00000048)"]),
    ("a pin provided twice: the pin and a provider",
     """#include "kvasir/Io/Types.hpp"
struct LedA { using Provides = brigand::list<Kvasir::Io::PinResource<0, 25>>; };
struct LedB { using Provides = brigand::list<Kvasir::Io::PinResource<0, 25>>; };
using Check = Kvasir::Startup::ResourceCheck<brigand::list<LedA, LedB>>;
static_assert(Kvasir::Startup::Diagnostics::Diagnose<Check>::value);
""", [],
     # one report per provider: gcc prints both, clang stops after the first (LedB)
     ["a hardware resource is provided (configured, owned) by two peripherals: Io pin 0.25 - Led"]),
    ("an interrupt rule: the index and its name",
     """struct TwiceRule {
    static constexpr auto kind = Kvasir::Startup::Diagnostics::NumberKind::interrupt;
    static constexpr std::string_view message = "two peripherals claim the same interrupt vector";
};
struct TheStartup {};
constexpr auto n = sizeof(Kvasir::Startup::Diagnostics::Report<TwiceRule, TheStartup, std::integral_constant<int, -1>>);
""", [],
     ["two peripherals claim the same interrupt vector: interrupt -1 (SysTick) - TheStartup"]),
    # the device interrupt names are a hook looked up when a report is built (StartUp/InterruptNames.hpp)
    ("an interrupt rule: a device index named by a hook specialised after Resources.hpp",
     """struct TwiceRule {
    static constexpr auto kind = Kvasir::Startup::Diagnostics::NumberKind::interrupt;
    static constexpr std::string_view message = "two peripherals claim the same interrupt vector";
};
struct TheStartup {};
template<typename... Ts>
struct Kvasir::Startup::Diagnostics::DeviceInterruptNames<Kvasir::Tag::User, Ts...> {
    static consteval std::string_view of(int index) { return index == 3 ? "TEST_IRQ3" : ""; }
};
constexpr auto n = sizeof(Kvasir::Startup::Diagnostics::Report<TwiceRule, TheStartup, std::integral_constant<int, 3>>);
""", [],
     ["two peripherals claim the same interrupt vector: interrupt 3 (TEST_IRQ3) - TheStartup"]),
    ("a passing resource check instantiates no name hook: specialised after it (a control: want None)",
     """struct LedA { using Provides = brigand::list<Kvasir::Io::PinResource<0, 25>>; };
struct LedB { using Provides = brigand::list<Kvasir::Io::PinResource<0, 26>>; };
using Check = Kvasir::Startup::ResourceCheck<brigand::list<LedA, LedB>>;
static_assert(Kvasir::Startup::Diagnostics::Diagnose<Check>::value);
template<typename... Ts>
struct Kvasir::Startup::Diagnostics::DeviceInterruptNames<Kvasir::Tag::User, Ts...> {
    static consteval std::string_view of(int) { return "late"; }
};
""".replace("struct LedA", '#include "kvasir/Io/Types.hpp"\nstruct LedA'), [], None),
    ("Resources.hpp includes no generated header: a poisoned peripherals/ first (a control: want None)",
     "constexpr int unused = 0;\n", ["-I@TESTS@/support/poisoned_generated"], None),
    ("an unclassified write-only bit not named",
     "void f() { apply(write(IcrstReg::latency, R::value<std::uint32_t, 5>())); }\n", [],
     ["it would write back the write-only field(s) bit 11 (0x00000800), which cannot be read"]),
    ("a toggle on a write-only register",
     "void f() { apply(toggle(SetResetReg::bs5)); }\n", [],
     ["a toggle needs the current value, and this register cannot be read"]),
    # apply()'s merge table and argument checks (Register/Apply.hpp, mergeOf)
    ("two toggles of the same bit in one step",
     "void f() { apply(toggle(CtrlReg::en), toggle(CtrlReg::en)); }\n", [],
     ["two toggles of the same bits in one step"]),
    ("a toggle and a runtime write of the same bit",
     "void f(unsigned v) { apply(toggle(CtrlReg::en), write(CtrlReg::en, Kvasir::Test::runtimeValue(v))); }\n", [],
     ["a toggle or an atomic write and a runtime write of the same bits in one apply()"]),
    ("an atomic clear and a runtime write of the same bit",
     "void f(unsigned v) { apply(R::atomic(clear(CtrlReg::en)), write(CtrlReg::en, Kvasir::Test::runtimeValue(v))); }\n", [],
     ["a toggle or an atomic write and a runtime write of the same bits in one apply()"]),
    ("runtime, literal, runtime of the same bits",
     "void f(unsigned a, unsigned b) { apply(write(SimpleTestReg::dat, Kvasir::Test::runtimeValue(a)),"
     " write(SimpleTestReg::cmd, R::value<std::uint32_t, 1>()), write(SimpleTestReg::dat, Kvasir::Test::runtimeValue(b))); }\n",
     [], ["two writes in one apply() cover the same bits of one register"]),
    ("a set and a clear of one bit in one step",
     "void f() { apply(set(CtrlReg::en), clear(CtrlReg::en)); }\n", [],
     ["two actions in one step write different values to the same bits of one register"]),
    ("a literal with ones under a runtime write of the same bits",
     "void f(unsigned v) { apply(write(CtrlReg::div, R::value<std::uint32_t, 2>()),"
     " write(CtrlReg::div, Kvasir::Test::runtimeValue(v))); }\n", [],
     ["a literal and a runtime write in one apply() cover the same bits and the literal writes ones there"]),
    ("a runtime value inside a list",
     "void f(unsigned v) { apply(Kvasir::MPL::list(write(CtrlReg::div, Kvasir::Test::runtimeValue(v)))); }\n", [],
     ["runtime values can only be executed in an apply, they cannot be stored in a list"]),
    # R::overrideDefaults with a runtime value: a bundle apply() takes alone (Register.hpp, Apply.hpp Defaulted)
    ("an overrideDefaults bundle alone (a control: want None)",
     "void f(unsigned v) { apply(SimpleTestReg::overrideDefaults(write(SimpleTestReg::dat, Kvasir::Test::runtimeValue(v)))); }\n",
     [], None),
    ("an overrideDefaults bundle inside a list",
     "void f(unsigned v) { apply(Kvasir::MPL::list(SimpleTestReg::overrideDefaults(write(SimpleTestReg::dat,"
     " Kvasir::Test::runtimeValue(v))))); }\n", [],
     ["R::overrideDefaults(...) with a runtime value is a bundle that apply() takes as its only argument"]),
    ("an overrideDefaults bundle next to another argument",
     "void f(unsigned v) { apply(SimpleTestReg::overrideDefaults(write(SimpleTestReg::dat, Kvasir::Test::runtimeValue(v))),"
     " set(CtrlReg::en)); }\n", [],
     ["R::overrideDefaults(...) with a runtime value is a bundle that apply() takes as its only argument"]),
    # R::overrideDefaults takes writes only: a read/toggle would drop that field's default write (Register.hpp)
    ("overrideDefaults of literal writes (a control: want None)",
     "void f() { apply(SimpleTestReg::overrideDefaults(set(SimpleTestReg::cmd), write(SimpleTestReg::dat,"
     " R::value<std::uint32_t, 3>()))); }\n", [], None),
    ("a read inside overrideDefaults",
     "void f(unsigned v) { apply(SimpleTestReg::overrideDefaults(write(SimpleTestReg::dat, Kvasir::Test::runtimeValue(v)),"
     " read(SimpleTestReg::cmd))); }\n", [],
     ["R::overrideDefaults(...) takes writes only (write, set, clear, reset): a read of bit 8 of register 0x00000010"]),
    ("a toggle inside a literal overrideDefaults",
     "void f() { apply(SimpleTestReg::overrideDefaults(set(SimpleTestReg::stop), toggle(SimpleTestReg::cmd))); }\n", [],
     ["R::overrideDefaults(...) takes writes only (write, set, clear, reset): a toggle of bit 8 of register 0x00000010"]),
    # Register::fromReset<R>: one store with R's reset value in the unnamed bits; a plain write of R beside it is refused
    ("fromReset writes of one register from two calls (a control: want None)",
     "void f() { apply(R::fromReset<ResetReg>(set(ResetReg::en)), R::fromReset<ResetReg>(clear(ResetReg::irq))); }\n",
     [], None),
    ("a fromReset write and a plain write of one register",
     "void f() { apply(R::fromReset<ResetReg>(set(ResetReg::en)), set(ResetReg::irq)); }\n", [],
     ["a Register::fromReset write and a plain write of the same register in one step"]),
    ("fromReset of a runtime write",
     "void f(unsigned v) { apply(R::fromReset<ResetReg>(write(ResetReg::div, Kvasir::Test::runtimeValue(v)))); }\n", [],
     ["Register::fromReset<R> takes literal writes of R's fields"]),
    ("fromReset of another register's field",
     "void f() { apply(R::fromReset<ResetReg>(set(CtrlReg::en))); }\n", [],
     ["Register::fromReset<R>: every action writes a field of R"]),
    ("an argument apply() does not take",
     "void f() { apply(set(CtrlReg::en), 5); }\n", [],
     ["one of the supplied arguments is not supported"]),
    ("result[field] of a field read in two steps",
     "unsigned f() { auto r = apply(read(SecondReg::data), R::sequencePoint, read(SecondReg::data));"
     " return r[SecondReg::data]; }\n", [],
     ["this field is read in more than one step of the apply()"]),
    ("result[field] of a field the apply() did not read, named register",
     "unsigned f() { auto r = apply(read(CTRLREG::div)); return r[CTRLREG::en]; }\n", [
     ],
     ["result[field]: field(s) en of test::ctrlreg (0x00000048) was not read in this apply()"]),
    ("get(field, result) of a field the apply() did not read, unnamed register",
     "unsigned f() { auto r = apply(read(PLAINREG::div)); return get(PLAINREG::en, r); }\n", [],
     ["result[field]: bit 0 of register 0x0000004C was not read in this apply()"]),
    ("the one-field conversion of a const result compiles (a control: want None)",
     "unsigned f() { auto const r = apply(read(CTRLREG::div)); std::uint32_t const v = r;"
     " return v + r[CTRLREG::div] + get(CTRLREG::div, r); }\n", [], None),
    # the Startup hook table (StartUp/ListRules.hpp, HookShapesHold)
    ("a Startup entry with right-shaped hooks compiles (a control: want None)",
     "struct Good { static void runtimeInit() {} static bool primarySync() { return true; }\n"
     "  static constexpr auto preEnableRuntimeInit = []() {}; };\n" + HOOK_ASSERT.format("Good"), [], None),
    ("a bool runtimeInit",
     "struct BoolInit { static bool runtimeInit() { return true; } };\n" +
     HOOK_ASSERT.format("BoolInit"), [],
     ["Startup entry BoolInit: its runtimeInit must be `static void runtimeInit()`"]),
    ("a void primarySync",
     "struct VoidSync { static void primarySync() {} };\n" +
     HOOK_ASSERT.format("VoidSync"), [],
     ["Startup entry VoidSync: its primarySync must be `static bool primarySync()`"]),
    ("a non-static preEnableRuntimeInit",
     "struct Member { void preEnableRuntimeInit() {} };\n" +
     HOOK_ASSERT.format("Member"), [],
     ["Startup entry Member: its preEnableRuntimeInit must be"]),
    # the shared check block: every rule names its offender, core 1's too
    ("Checks: a clean primary and secondary list compile (a control: want None)",
     CHECKS +
     "static_assert(checked<C::Role::primary, 0, Dup, WithIsr<3>, Core1<WithIsr<5>>>);\n"
     "static_assert(checked<C::Role::secondary, 1, WithIsr<5>>);\n"
     "static_assert(image<Dup, WithIsr<3>, Core1<WithIsr<5>>>);\n", [], None),
    ("Checks: a type twice in the primary list",
     CHECKS +
     "static_assert(checked<C::Role::primary, 0, Dup, WithIsr<3>, Dup>);\n", [],
     ["a peripheral is listed twice in one Startup list (its init steps would run twice): core 0 - Dup"]),
    ("Checks: a type twice in a SecondaryCore's list",
     CHECKS + "static_assert(checked<C::Role::secondary, 1, Dup, Dup>);\n", [],
     ["a peripheral is listed twice in one Startup list (its init steps would run twice): core 1 - Dup"]),
    ("Checks: a non-peripheral on core 1",
     CHECKS + "static_assert(checked<C::Role::secondary, 1, Dup, Cfg>);\n", [],
     ["a type in the Startup list is not a peripheral", ": core 1 - Cfg"]),
    ("Checks: an interrupt installed on both cores",
     CHECKS + "struct Other5 { using Isr = brigand::list<FakeIsr<5>>; };\n"
     "static_assert(image<WithIsr<5>, Core1<Other5>>);\n", [],
     ["an interrupt is installed in both cores' vector tables", ": interrupt 5", "- Core1<Other5>"]),
    ("Checks: a dependency listed after the SecondaryCore",
     CHECKS + "struct Ref { static constexpr int initStepPinConfig = 0; };\n"
     "struct NeedsRef { using Claims = brigand::list<Kvasir::Startup::ListedOn<Ref, 0>>; };\n"
     "static_assert(image<Core1<NeedsRef>, Ref>);\n", [],
     ["a SecondaryCore's peripheral needs a core 0 peripheral whose init has not run by the launch",
      "Core1<NeedsRef> - Ref"]),
    # the boot report rules: the user backend needs Boot::Report / ReportInMain (BootReportListed), a guard comes before
    # the report (GuardBeforeReport); a group is read through Entries, as Startup does
    ("Checks: the backend with the report, the marker or a group holding it compiles (a control: want None)",
     CHECKS + BOOT +
     "static_assert(checkedL<C::Role::primary, 0, Log, Report>);\n"
     "static_assert(checkedL<C::Role::primary, 0, InMain, Log>);\n"
     "static_assert(checkedL<C::Role::primary, 0, Log, Bundle>);\n"
     "static_assert(checkedL<C::Role::primary, 0, Dup>);\n"
     "static_assert(checkedL<C::Role::secondary, 1, Log>);\n", [], None),
    ("Checks: the user backend without a boot report",
     CHECKS + BOOT +
     "static_assert(checkedL<C::Role::primary, 0, Log, Dup>);\n", [],
     ["a Startup list with the user log backend names no boot report", "Kvasir::Boot::ReportInMain",
      ": core 0 - Log"]),
    ("Checks: a boot guard after the report",
     CHECKS + BOOT +
     "static_assert(checkedL<C::Role::primary, 0, Log, Report, Guard>);\n", [],
     ["a BootGuard::Guard after Boot::Report in a Startup list", ": core 0 - Guard"]),
    ("Checks: a Health::Service<> after the report compiles (a control: want None)",
     CHECKS + BOOT + "static_assert(checkedL<C::Role::primary, 0, Log, Report, Service>);\n", [], None),
    ("Checks: a Health::Service<> before the report",
     CHECKS + BOOT +
     "static_assert(checkedL<C::Role::primary, 0, Log, Service, Bundle>);\n", [],
     ["a Health::Service<> before Boot::Report in a Startup list", ": core 0 - Service"]),
    ("Checks: a watchdog before the Health::Service<> compiles (a control: want None)",
     CHECKS + BOOT + "static_assert(checkedL<C::Role::primary, 0, Log, Report, Dog, Service>);\n", [], None),
    ("Checks: a watchdog after the Health::Service<>",
     CHECKS + BOOT +
     "static_assert(checkedL<C::Role::primary, 0, Log, Report, Service, Dog>);\n", [
     ],
     ["a watchdog after Health::Service<> in a Startup list", ": core 0 - Dog"]),
    # the image's CRC engine set (Checks.hpp UnlistedEngineSet): listed where it claims hardware
    ("Checks: a claiming CRC engine set listed on core 0 or core 1, a set that claims nothing, no hook (a control: "
     "want None)",
     CHECKS + CRCSET +
     "static_assert(imageC<ClaimingSet, Dup, ClaimingSet>);\n"
     "static_assert(imageC<ClaimingSet, Dup, Core1<ClaimingSet>>);\n"
     "static_assert(imageC<SoftwareSet, Dup>);\n"
     "static_assert(image<Dup>);\n", [], None),
    ("Checks: a CRC engine set that claims hardware and is in no list",
     CHECKS + CRCSET +
     "static_assert(imageC<ClaimingSet, Dup, Core1<WithIsr<5>>>);\n", [],
     ["the image's CRC engine set (Kvasir::Crc::ImageEngines<Kvasir::Tag::User>) claims hardware but is in no "
      "Startup list", "- ClaimingSet"]),
    # the chip's own entries (Startup::ChipEntries): Startup adds them, a list that names one by hand is refused
    # (ChipEntryNotListed, asserted by Startup over the list as written)
    ("Checks: a list without a chip entry (a control: want None)",
     CHECKS +
     "struct ChipPiece { static constexpr bool chipEntry = true; static void postMemoryInit() {} };\n"
     "static_assert(C::reported<C::ChipEntryListed, C::Detail::OnCore<0, "
     "Kvasir::Startup::ListRules::ListedChipEntries<brigand::list<Dup>>>>);\n", [], None),
    ("Checks: a chip entry listed by hand",
     CHECKS +
     "struct ChipPiece { static constexpr bool chipEntry = true; static void postMemoryInit() {} };\n"
     "static_assert(C::reported<C::ChipEntryListed, C::Detail::OnCore<0, "
     "Kvasir::Startup::ListRules::ListedChipEntries<brigand::list<Dup, ChipPiece>>>>);\n", [],
     ["an entry the chip package adds to every Startup list itself (Startup::ChipEntries", ": core 0 - ChipPiece"]),
    # an entry with an IsrTransform (IsrProfiling): one per list, on either core
    ("Checks: two IsrTransforms in one list",
     CHECKS +
     "template<int N> struct Tr { template<typename I, unsigned C> using IsrTransform = I; "
     "static void preEnableRuntimeInit() {} };\n"
     "static_assert(checked<C::Role::primary, 0, Tr<1>, Dup, Tr<2>>);\n", [],
     ["two entries in one Startup list rewrite its ISR list (IsrTransform", ": core 0 - Tr<2>"]),
    ("Checks: two IsrTransforms in a SecondaryCore's list",
     CHECKS +
     "template<int N> struct Tr { template<typename I, unsigned C> using IsrTransform = I; "
     "static void preEnableRuntimeInit() {} };\n"
     "static_assert(checked<C::Role::secondary, 1, Tr<1>, Dup, Tr<2>>);\n", [],
     ["two entries in one Startup list rewrite its ISR list (IsrTransform", ": core 1 - Tr<2>"]),
]


CASES = [Case(name, body, want, flags=flags)
         for name, body, flags, want in ROWS]

if __name__ == "__main__":
    sys.exit(run(HEAD, CASES, flags=["-DKVASIR_REGISTER_MOCK"]))
