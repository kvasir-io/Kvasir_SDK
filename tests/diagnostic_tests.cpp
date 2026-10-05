// Kvasir::Diagnostic (kvasir/Util/Diagnostic.hpp): the Text writer, typeName, the abbreviation, and the lazy rule -
// lazy<true, G>() never instantiates G's operator() (a G whose body cannot compile is fine), lazy<false, G>() is its
// text. The compile errors themselves are diagnostics_must_fail/.
#include "kvasir/Util/Diagnostic.hpp"

#include "test_harness.hpp"

#include <string_view>

namespace D = Kvasir::Diagnostic;

namespace Outer {
struct Plain {};

template<typename T, int N>
struct Tmpl {};

struct Holder {
    struct Nested {};
};
}   // namespace Outer

namespace {
struct Hidden {};

// instantiating its operator() is an error: only a failing assert may do that
template<typename T>
struct Broken {
    consteval D::Text operator()() const {
        static_assert(sizeof(T) == 0, "instantiated although the assert holds");
        return {};
    }
};

struct Fine {
    consteval D::Text operator()() const {
        D::Text t;
        t << "register ";
        t.hex(0x4010000CU);
        return t;
    }
};

constexpr std::string_view text(D::Text const& t) { return t.view(); }

constexpr D::Text hexOf(std::uint32_t v) {
    D::Text t;
    t.hex(v);
    return t;
}

constexpr D::Text bitsOf(std::uint32_t v) {
    D::Text t;
    t.bits(v);
    return t;
}

constexpr D::Text abbreviated(std::string_view s) {
    D::Text t;
    D::appendAbbreviated(t, s);
    return t;
}
}   // namespace

// the lazy rule, at compile time: Broken<int>'s operator() is never instantiated
static_assert(D::lazy<true,
                      Broken<int>>()
                .size()
              == 0);
static_assert(D::Check<true,
                       Broken<char>>::value,
              "a passing check never touches the generator");
static_assert(text(D::lazy<false,
                           Fine>())
              == "register 0x4010000C");
static_assert(text(hexOf(0x4010000CU)) == "0x4010000C");
static_assert(text(bitsOf(0xFFFF0000U)) == "31:16" && text(bitsOf(0x80U)) == "7"
              && text(bitsOf(0xFFFFFFFFU)) == "31:0");
static_assert(text(bitsOf(0x101U)) == "0x00000101",
              "not one run: the hex value");

int main() {
    using Kvasir::Test::test;

    test("typeName: plain, template, nested, anonymous namespace");
    CHECK(D::typeName<Outer::Plain>() == "Outer::Plain");
    CHECK(D::typeName<Outer::Tmpl<int, 3>>() == "Outer::Tmpl<int, 3>");
    CHECK(D::typeName<Outer::Holder::Nested>() == "Outer::Holder::Nested");
    auto const hidden = D::typeName<Hidden>();
    CHECK(hidden == "(anonymous namespace)::Hidden" || hidden == "{anonymous}::Hidden");

    test("Text: dec, signed, a full buffer is cut, not overrun");
    {
        D::Text t;
        t.dec(0).sdec(-42) << " ";
        t.dec(18446744073709551615ULL);
        CHECK(t.view() == "0-42 18446744073709551615");
        D::Text full;
        for(int i = 0; i != 100; ++i) { full << "0123456789"; }
        CHECK(full.size() == full.chars.size());
    }

    test("abbreviation: long argument lists become <...>, short ones stay");
    CHECK(text(abbreviated("HW::LedPinConfig")) == "HW::LedPinConfig");
    CHECK(text(abbreviated("Outer::Tmpl<int, 3>")) == "Outer::Tmpl<int, 3>");
    CHECK(text(abbreviated(
            "Kvasir::UART::UartStream<HW::UartConfig, Kvasir::DMA::DmaBase<DmaConfig>>::Isr"))
          == "Kvasir::UART::UartStream<...>::Isr");

    test("the macro: a holding assert compiles with a generator whose body cannot");
    {
        static constexpr bool ok = true;
        KVASIR_STATIC_ASSERT(ok, (Broken<long>), "never shown");
        CHECK(true);
    }

    return Kvasir::Test::report();
}
