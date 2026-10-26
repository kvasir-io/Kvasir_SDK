// RegisterFmt.hpp through remote_fmt's own encoder and parser: a register line and a Flags<> line are printed into a
// byte buffer and decoded as the printer decodes them. Built twice (tests/CMakeLists.txt): without the catalog
// (REMOTE_FMT_USE_CATALOG=false: the register's descriptor - format string, masks, value names - travels inline with
// the line) and with it (the descriptor is a type entry of the catalog, resolved through the json this build's
// catalog tools wrote). Either way only the raw word goes out. Both must give the same text.
//
// The registers are hand-written in the shape svd_converter emits (resetValue, fields, a ValueNames struct per
// enumerated field, fmt_string, field_names, field_dims, apply_fields and apply_fields_with_dim through
// Register/Views.hpp). Their reads go through the register mock.
#include "kvasir/Register/Register.hpp"

#include "kvasir/Register/RegisterFmt.hpp"
#include "kvasir_test.hpp"
#include "remote_fmt/catalog.hpp"
#include "remote_fmt/catalog_helpers.hpp"
#include "remote_fmt/parser.hpp"
#include "remote_fmt/remote_fmt.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

using namespace sc::literals;

#ifndef REMOTE_FMT_TEST_CATALOG_JSON
    #define REMOTE_FMT_TEST_CATALOG_JSON ""
#endif

// catalog<>() is defined by the generated catalog
#ifdef __clang__
    #pragma clang diagnostic ignored "-Wundefined-func-template"
#endif

// a named namespace: an unused FULLREGISTER in an unnamed one is a warning
namespace regfmt {
using namespace Kvasir::Register;

// A status register: three single-bit flags around a multi-bit counter.
struct StatusReg {
    using Addr = Address<0x40, 0x00000000, 0x00000000, std::uint32_t>;

    static constexpr std::uint32_t resetValue = 0;
    static constexpr FieldLocation<Addr, maskFromRange(31, 0), ReadWriteAccess, std::uint32_t>
      FULLREGISTER{};
    static constexpr FieldLocation<Addr, maskFromRange(0, 0), ReadWriteAccess, std::uint32_t>
      rxDone{};
    static constexpr FieldLocation<Addr, maskFromRange(1, 1), ReadWriteAccess, std::uint32_t>
      txDone{};
    static constexpr FieldLocation<Addr, maskFromRange(7, 4), ReadWriteAccess, std::uint32_t>
      level{};
    static constexpr FieldLocation<Addr, maskFromRange(8, 8), ReadWriteAccess, std::uint32_t>
      overrun{};

    using fields = Fields<decltype(rxDone), decltype(txDone), decltype(level), decltype(overrun)>;

    static constexpr bool             glaze_reflect = false;   // as svd_converter emits it
    static constexpr std::string_view fmt_string{
      "test::status(\n\"rx_done\": 0x{:x}\n\"tx_done\": 0x{:x}\n\"level\": 0x{:x}\n\"overrun\": "
      "0x{:x})"};
    static constexpr auto field_names
      = std::to_array<std::string_view>({"rx_done", "tx_done", "level", "overrun"});
    static constexpr std::array<unsigned, 0> field_dims{};

    template<typename Func>
    static constexpr auto apply_fields(Func&& func) {
        return applyFields<StatusReg>(std::forward<Func>(func));
    }

    template<typename Func>
    static constexpr auto apply_fields_with_dim(Func&& func) {
        return applyFieldsWithDim<StatusReg>(std::forward<Func>(func));
    }
};

// A control register of channel 3 of a cluster: a dim in front, an enumeration (value 3 has no name), a number.
struct ChanCtrlReg {
    using Addr = Address<0x44, 0x00000000, 0x00000000, std::uint32_t>;

    struct MODEValueNames {
        static consteval auto get() {
            return std::to_array<ValueName>({
              {0,  "off"},
              {1, "slow"},
              {2, "fast"}
            });
        }
    };

    enum class MODEVal : std::uint32_t { off = 0, slow = 1, fast = 2 };

    friend constexpr MODEValueNames kvasirValueNames(MODEVal) { return {}; }

    static constexpr std::uint32_t resetValue = 0;
    static constexpr FieldLocation<Addr, maskFromRange(31, 0), ReadWriteAccess, std::uint32_t>
      FULLREGISTER{};
    static constexpr FieldLocation<Addr, maskFromRange(1, 0), ReadWriteAccess, MODEVal> mode{};
    static constexpr FieldLocation<Addr, maskFromRange(15, 8), ReadWriteAccess, std::uint32_t>
      div{};

    using fields = Fields<decltype(mode), decltype(div)>;

    static constexpr bool             glaze_reflect = false;   // as svd_converter emits it
    static constexpr std::string_view fmt_string{
      "test::chan[{}]::ctrl(\n\"mode\": {}\n\"div\": 0x{:x})"};
    static constexpr auto field_names = std::to_array<std::string_view>({"mode", "div"});
    static constexpr std::array<unsigned, 1> field_dims{3};

    template<typename Func>
    static constexpr auto apply_fields(Func&& func) {
        return applyFields<ChanCtrlReg>(std::forward<Func>(func));
    }

    template<typename Func>
    static constexpr auto apply_fields_with_dim(Func&& func) {
        return applyFieldsWithDim<ChanCtrlReg>(std::forward<Func>(func));
    }
};

// A register a person wrote: fmt_string and apply_fields only.
struct PlainReg {
    using Addr = Address<0x48, 0x00000000, 0x00000000, std::uint32_t>;
    static constexpr FieldLocation<Addr, maskFromRange(3, 0), ReadWriteAccess, std::uint32_t>
                                      nibble{};
    static constexpr bool             glaze_reflect = false;   // as svd_converter emits it
    static constexpr std::string_view fmt_string{"test::plain(\n\"nibble\": {})"};

    template<typename Func>
    static constexpr auto apply_fields(Func&& func) {
        return std::invoke(std::forward<Func>(func),
                           static_cast<std::uint32_t>(get<0>(apply(read(nibble)))));
    }

    template<typename Func>
    static constexpr auto apply_fields_with_dim(Func&& func) {
        return apply_fields(std::forward<Func>(func));
    }
};

struct NoFmtString {
    template<typename Func>
    static constexpr auto apply_fields(Func&& func) {
        return std::invoke(std::forward<Func>(func), 1U);
    }
};

struct NoDims {   // StatusReg without field_dims: printable, flag-printable, not tabled
    using Addr                                      = StatusReg::Addr;
    using fields                                    = StatusReg::fields;
    static constexpr bool             glaze_reflect = false;   // as svd_converter emits it
    static constexpr std::string_view fmt_string{StatusReg::fmt_string};
    static constexpr auto             field_names = StatusReg::field_names;

    template<typename Func>
    static constexpr auto apply_fields(Func&& func) {
        return StatusReg::apply_fields(std::forward<Func>(func));
    }
};

static_assert(PrintableRegister<StatusReg> && TabledRegister<StatusReg>
              && FlagPrintableRegister<StatusReg>);
static_assert(PrintableRegister<ChanCtrlReg> && TabledRegister<ChanCtrlReg>
              && FlagPrintableRegister<ChanCtrlReg>);
static_assert(PrintableRegister<PlainReg> && !TabledRegister<PlainReg>
              && !FlagPrintableRegister<PlainReg>);
static_assert(!PrintableRegister<NoFmtString> && !TabledRegister<NoFmtString>);
static_assert(PrintableRegister<NoDims> && !TabledRegister<NoDims>
              && FlagPrintableRegister<NoDims>);

struct VectorBackend {
    std::vector<std::byte> memory;

    void write(std::span<std::byte const> data) {
        memory.insert(memory.end(), data.begin(), data.end());
    }
};

// Without the catalog nothing has an id; with it, every string this build catalogs, as the generator wrote them.
std::unordered_map<std::uint16_t,
                   std::string> const&
strings() {
    static auto const& map = *new std::unordered_map<std::uint16_t, std::string>{[] {
        if constexpr(remote_fmt::useCatalog) {
            auto read = remote_fmt::readCatalogFile(REMOTE_FMT_TEST_CATALOG_JSON);
            if(!read) {
                Kvasir::Test::fail("the catalog json reads");
                return std::unordered_map<std::uint16_t, std::string>{};
            }
            return std::move(read->strings);
        } else {
            return std::unordered_map<std::uint16_t, std::string>{};
        }
    }()};
    return map;
}

template<typename... Args>
std::optional<std::string> roundTrip(auto fmt,
                                     Args const&... args) {
    remote_fmt::Printer<VectorBackend> printer{};
    printer.print(fmt, args...);
    auto const& buffer = printer.getComBackend().memory;
    std::string error;
    auto const  parsed = remote_fmt::parseMessage(std::span{buffer},
                                                  strings(),
                                                  [&](std::string_view e) { error = e; });
    if(!parsed.message) { std::print("    parse error: {}\n", error); }
    return parsed.message;
}

template<typename R>
void checkFlags(unsigned             value,
                std::string_view     want,
                std::source_location loc = std::source_location::current()) {
    auto const got = roundTrip("flags {}"_sc, Flags<R>{value});
    if(got != std::optional<std::string>{"flags " + std::string{want}}) {
        Kvasir::Test::failf(loc,
                            "Flags of 0x{:X}: got '{}', want 'flags {}'",
                            value,
                            got.value_or("<none>"),
                            want);
    }
}

void flags() {
    Kvasir::Test::test(remote_fmt::useCatalog ? "flags, catalog" : "flags, no catalog");
    checkFlags<StatusReg>(0x101, "rx_done|overrun");   // in field order
    checkFlags<StatusReg>(0x003, "rx_done|tx_done");
    checkFlags<StatusReg>(0x000, "none");   // nothing set: one "none"
    checkFlags<StatusReg>(0x0F0, "none");   // a multi-bit field is a value, not a flag
    checkFlags<StatusReg>(0x1F2, "tx_done|overrun");
    checkFlags<StatusReg>(0xFFFFFE00, "none");   // bits no field has
    checkFlags<NoDims>(0x100, "overrun");
    // the register is not read: Flags formats the value it was given
    CHECK(Kvasir::Test::recorder.actions.empty());
}

void wholeRegister() {
    Kvasir::Test::test(remote_fmt::useCatalog ? "whole register, catalog"
                                              : "whole register, no catalog");
    auto& rec = Kvasir::Test::recorder;

    rec.setReadValues(0x40, {0x1A3, 0x1A3});
    CHECK(
      roundTrip("{}"_sc, StatusReg{})
      == std::optional<std::string>{
        "test::status(\n\"rx_done\": 0x1\n\"tx_done\": 0x1\n\"level\": 0xa\n\"overrun\": 0x1)"});

    // the dim first, then the enumeration by its value's name, then the number
    rec.setReadValues(0x44, {0x2A01, 0x2A01});
    CHECK(roundTrip("{}"_sc, ChanCtrlReg{})
          == std::optional<std::string>{"test::chan[3]::ctrl(\n\"mode\": slow\n\"div\": 0x2a)"});
    rec.setReadValues(0x44, {0x0002, 0x0002});
    CHECK(roundTrip("{}"_sc, ChanCtrlReg{})
          == std::optional<std::string>{"test::chan[3]::ctrl(\n\"mode\": fast\n\"div\": 0x0)"});

    // one read of the register per line
    rec.actions.clear();
    rec.setReadValue(0x44, 0x0100);
    static_cast<void>(roundTrip("{}"_sc, ChanCtrlReg{}));
    CHECK_EQ(rec.actions.size(), 1);

    rec.setReadValue(0x48, 0xB);
    CHECK(roundTrip("{}"_sc, PlainReg{})
          == std::optional<std::string>{"test::plain(\n\"nibble\": 11)"});
}

// An enumeration value without a name: the host prints the number.
void unnamedEnumValue() {
    Kvasir::Test::test(remote_fmt::useCatalog ? "enumeration value without a name, catalog"
                                              : "enumeration value without a name, no catalog");
    Kvasir::Test::recorder.setReadValue(0x44, 0x0503);
    CHECK(roundTrip("{}"_sc, ChanCtrlReg{})
          == std::optional<std::string>{"test::chan[3]::ctrl(\n\"mode\": 3\n\"div\": 0x5)"});
}

// One raw word on the wire, whatever the register: the frame is start, id, the varint, [CRC], end
void rawWord() {
    Kvasir::Test::test("a register sends its raw word only");
    Kvasir::Test::recorder.setReadValue(0x44, 0x0503);
    remote_fmt::Printer<VectorBackend> printer{};
    printer.print("{}"_sc, ChanCtrlReg{});
    auto const& m = printer.getComBackend().memory;
    if constexpr(remote_fmt::useCatalog) {
        CHECK_EQ(
          m.size(),
          std::size_t{3 + 2 + (REMOTE_FMT_FRAME_CHECK ? 1 : 0) + 1});   // 0x503: two LEB128 bytes
    }
    remote_fmt::Printer<VectorBackend> flagsPrinter{};
    flagsPrinter.print("{}"_sc, Flags<StatusReg>{0x101});
    if constexpr(remote_fmt::useCatalog) {
        CHECK_EQ(flagsPrinter.getComBackend().memory.size(),
                 std::size_t{3 + 2 + (REMOTE_FMT_FRAME_CHECK ? 1 : 0) + 1});
    }
}

}   // namespace regfmt

#if defined( \
  KVASIR_TEST_GENERATED_REGISTERS)   // tests/CMakeLists.txt: the converter's fixture headers are built
    #include "fixture_plain/CTRLP.hpp"
    #include "kvasir/Register/Diagnostic.hpp"

// The same through registers svd_converter generated from its golden fixture (tests/golden/features.svd, base
// 0x40000000): the safety net for moving the views out of the generated text (the views must give these bytes).
namespace regfmt_generated {
using P = Kvasir::Peripheral::CTRLP::Registers<>;

template<typename R>
void checkFlags(unsigned             value,
                std::string_view     want,
                std::source_location loc = std::source_location::current()) {
    auto const got = regfmt::roundTrip(
      "flags {}"_sc,
      Kvasir::Register::Flags<R>{static_cast<typename R::Addr::RegType>(value)});
    if(got != std::optional<std::string>{"flags " + std::string{want}}) {
        Kvasir::Test::failf(loc,
                            "Flags of 0x{:X}: got '{}', want 'flags {}'",
                            value,
                            got.value_or("<none>"),
                            want);
    }
}

template<typename R>
constexpr Kvasir::Diagnostic::Text fieldsOf(unsigned mask) {
    Kvasir::Diagnostic::Text t;
    Kvasir::Register::Diagnostic::fieldsIn<R>(t, mask);
    return t;
}

static_assert(fieldsOf<P::TRIGGER>(0x103).view() == "start, flip, level");
static_assert(fieldsOf<P::CH<1>>(0x100).view() == "en[0]");
static_assert(fieldsOf<P::CH<1>>(0x10000).view() == "bit 16");

void flags() {
    Kvasir::Test::test(remote_fmt::useCatalog ? "generated flags, catalog"
                                              : "generated flags, no catalog");
    checkFlags<P::INTFLAG>(0x0, "none");
    checkFlags<P::INTFLAG>(0x2, "error");
    checkFlags<P::INTFLAG>(0x3, "ready|error");
    checkFlags<P::TRIGGER>(0xF06, "flip|done");   // LEVEL is a value, not a flag
    checkFlags<P::CH<2>>(0x3FF, "en[0]|en[1]");   // a field cluster's elements
}

void wholeRegister() {
    Kvasir::Test::test(remote_fmt::useCatalog ? "generated whole register, catalog"
                                              : "generated whole register, no catalog");
    auto& rec = Kvasir::Test::recorder;
    // a register cluster with a field cluster: the dim first, then each element
    rec.setReadValues(0x40000024, {0x2A5, 0x2A5});
    CHECK(regfmt::roundTrip("{}"_sc, P::CH<1>{})
          == std::optional<std::string>{
            "ctrlp::ch[1](\n\"threshold\": 0xa5\n\"en[0]\": 0x0\n\"en[1]\": 0x1)"});
    // a register group: its index in front
    rec.setReadValues(0x40000050, {0x9, 0x9});
    CHECK(regfmt::roundTrip("{}"_sc, P::GRP<1>::CFG{})
          == std::optional<std::string>{"ctrlp::grp[1]::cfg(\n\"sel\": 0x9)"});
}
}   // namespace regfmt_generated
#endif

int main() {
    regfmt::flags();
    regfmt::wholeRegister();
    regfmt::unnamedEnumValue();
    regfmt::rawWord();
#ifdef KVASIR_TEST_GENERATED_REGISTERS
    regfmt_generated::flags();
    regfmt_generated::wholeRegister();
#endif
    return Kvasir::Test::report();
}
