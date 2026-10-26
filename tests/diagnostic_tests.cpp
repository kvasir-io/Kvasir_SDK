// Kvasir::Diagnostic (kvasir/Util/Diagnostic.hpp): FixedString, the Text writer (fixed1, sdec with plus), Message,
// typeName / valueName / lastNames, the abbreviation, and the lazy rule -
// Check<true, G> never instantiates G's operator() (a G whose body cannot compile is fine). The compile errors
// themselves are diagnostics_must_fail/.
#include "kvasir/Util/Diagnostic.hpp"

#include "test_harness.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace D = Kvasir::Diagnostic;

namespace Outer {
struct Plain {};

template<typename T, int N>
struct Tmpl {};

struct Holder {
    struct Nested {};
};

template<typename T>
struct W {};

inline void freeFunction() {}

template<int N>
struct Bus {
    static void turn() {}
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

constexpr D::Text fixed1Of(std::uint64_t num,
                           std::uint64_t den) {
    D::Text t;
    t.fixed1(num, den);
    return t;
}

constexpr D::Text plusOf(std::int64_t v) {
    D::Text t;
    t.sdec(v, true);
    return t;
}

struct Number {
    consteval D::Text operator()() const {
        D::Text t;
        t << "n = ";
        t.dec(42);
        return t;
    }
};

template<D::FixedString S>
constexpr std::string_view viewOf() {
    return S.view();
}

template<D::FixedString S>
constexpr std::size_t lengthOf() {
    return S.length;
}

constexpr D::Text lastNamesOf(std::string_view name,
                              std::size_t      parts = 2) {
    D::Text               t;
    std::array<char, 128> buf{};
    t << std::string_view{buf.data(), D::lastNames(name, buf.data(), parts)};
    return t;
}
}   // namespace

// FixedString as a template argument
static_assert(viewOf<"usb">() == "usb" && lengthOf<"usb">() == 3 && lengthOf<"">() == 0);
// fixed1: one decimal, rounded
static_assert(text(fixed1Of(1'171'875,
                            10))
              == "117187.5");
static_assert(text(fixed1Of(8'000'000,
                            258))
              == "31007.8");
static_assert(text(fixed1Of(3'000'000,
                            1))
              == "3000000.0");
// sdec with plus: "+" before every value >= 0, zero included (Prescaler's ppm error)
static_assert(text(plusOf(64)) == "+64" && text(plusOf(-142857)) == "-142857"
              && text(plusOf(0)) == "+0");
// Message: a generator's text as an object
static_assert(D::Message<Number>{}.view() == "n = 42" && D::Message<Number>{}.size() == 6);
// lastNames: template arguments dropped, the last two names kept
static_assert(text(lastNamesOf("Ns::Class<A<B>>::fn")) == "Class::fn");
static_assert(text(lastNamesOf("fn")) == "fn" && text(lastNamesOf("Class::fn")) == "Class::fn");
static_assert(text(lastNamesOf("A::B::C::fn",
                               3))
                == "B::C::fn"
              && text(lastNamesOf("A::B::fn",
                                  1))
                   == "fn");

// the lazy rule, at compile time: Broken<char>'s operator() is never instantiated
static_assert(D::Check<true,
                       Broken<char>>::value,
              "a passing check never touches the generator");
static_assert(text(Fine{}()) == "register 0x4010000C");
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

    test("typeName: a name with ']' is not cut (gcc cut it at the first ']' before)");
#ifdef __clang__
    CHECK(D::typeName<Outer::W<int[4]>>() == "Outer::W<int[4]>");
#else
    CHECK(D::typeName<Outer::W<int[4]>>() == "Outer::W<int [4]>");
#endif

    test("valueName: a function's name, no '&'");
    CHECK(D::valueName<&Outer::freeFunction>() == "Outer::freeFunction");
    CHECK(D::valueName<&Outer::Bus<0>::turn>() == "Outer::Bus<0>::turn");

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
        KVASIR_STATIC_ASSERT(ok, (Broken<long>));
        CHECK(true);
    }

    return Kvasir::Test::report();
}
