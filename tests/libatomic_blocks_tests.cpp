// Tests for the atomic shim's operations (kvasir/StartUp/Runtime/LibAtomicBlocks.hpp), the bodies behind the
// __atomic_* / __sync_* entry points of StartUp/Runtime/LibAtomic.hpp. The property the RP2040's Spinlock and
// std::atomic_flag rely on is pinned here: an exchange that changes nothing writes nothing - a plain store from the
// other core (a flag's clear(), outside the shim's lock) between the exchange's read and its write must not be undone.
// A Counting access counts the writes; Word<T> and Bytes are checked on real objects.
#include "kvasir/StartUp/Runtime/LibAtomicBlocks.hpp"
#include "test_harness.hpp"

#include <array>
#include <cstdint>
#include <cstring>

namespace Shim = Kvasir::Atomic::Shim;

namespace {
// Word<T> with its writes counted
template<typename T>
struct Counting {
    int* writes;

    void read(void const volatile* obj,
              void*                to) const {
        Shim::Word<T>::read(obj, to);
    }

    void write(void volatile* obj,
               void const*    from) const {
        ++*writes;
        Shim::Word<T>::write(obj, from);
    }

    [[nodiscard]] bool holds(void const volatile* obj,
                             void const*          value) const {
        return Shim::Word<T>::holds(obj, value);
    }

    [[nodiscard]] bool same(void const* a,
                            void const* b) const {
        return Shim::Word<T>::same(a, b);
    }
};

void exchangeWritesOnlyOnChange() {
    Kvasir::Test::test("exchange writes only on change");
    int writes{};
    unsigned char volatile flag = 1;   // held
    unsigned char const set     = 1;
    unsigned char       old{};
    // a waiter's test_and_set on a held flag: reads 1, must not write
    Shim::exchange(Counting<unsigned char>{&writes}, &flag, &set, &old);
    CHECK_EQ(old, 1);
    CHECK_EQ(writes, 0);
    CHECK_EQ(flag, 1);
    // the owner's clear(), then the next test_and_set: reads 0, writes 1 once
    flag = 0;
    Shim::exchange(Counting<unsigned char>{&writes}, &flag, &set, &old);
    CHECK_EQ(old, 0);
    CHECK_EQ(writes, 1);
    CHECK_EQ(flag, 1);
}

void exchangeWord() {
    Kvasir::Test::test("exchange Word<T>");
    std::uint32_t volatile word = 0x1234'5678U;
    std::uint32_t const next    = 0xcafe'f00dU;
    std::uint32_t       old{};
    Shim::exchange(Shim::Word<std::uint32_t>{}, &word, &next, &old);
    CHECK_EQ(old, 0x1234'5678U);
    CHECK_EQ(word, 0xcafe'f00dU);
    std::uint64_t volatile wide = 1;
    std::uint64_t const big     = 0x1'0000'0002ULL;
    std::uint64_t       was{};
    Shim::exchange(Shim::Word<std::uint64_t>{}, &wide, &big, &was);
    CHECK_EQ(was, 1U);
    CHECK_EQ(wide, 0x1'0000'0002ULL);
}

void exchangeBytesWritesOnlyOnChange() {
    Kvasir::Test::test("exchange Bytes writes only on change");

    struct Big {
        std::array<std::uint8_t, 12> b;
    };

    int writes{};
    Big obj{
      {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}
    };
    Big        same = obj;
    Big        old{};
    auto const counting = [&writes](Shim::Bytes bytes) {
        struct C {
            Shim::Bytes bytes;
            int*        writes;

            void read(void const volatile* o,
                      void*                to) const {
                bytes.read(o, to);
            }

            void write(void volatile* o,
                       void const*    from) const {
                ++*writes;
                bytes.write(o, from);
            }

            [[nodiscard]] bool holds(void const volatile* o,
                                     void const*          v) const {
                return bytes.holds(o, v);
            }

            [[nodiscard]] bool same(void const* a,
                                    void const* b) const {
                return bytes.same(a, b);
            }
        };
        return C{bytes, &writes};
    };
    Shim::exchange(counting(Shim::Bytes{sizeof(Big)}), &obj, &same, &old);
    CHECK_EQ(writes, 0);
    CHECK(std::memcmp(&old, &obj, sizeof(Big)) == 0);
    Big other{
      {12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1}
    };
    Shim::exchange(counting(Shim::Bytes{sizeof(Big)}), &obj, &other, &old);
    CHECK_EQ(writes, 1);
    CHECK(std::memcmp(&old, &same, sizeof(Big)) == 0);
    CHECK(std::memcmp(&obj, &other, sizeof(Big)) == 0);
}

void compareExchange() {
    Kvasir::Test::test("compareExchange");
    std::uint16_t volatile obj = 7;
    std::uint16_t       expected{7};
    std::uint16_t const desired{9};
    CHECK(Shim::compareExchange(Shim::Word<std::uint16_t>{}, &obj, &expected, &desired));
    CHECK_EQ(obj, 9);
    CHECK_EQ(expected, 7);
    // a miss: nothing stored, `expected` = what the object holds
    expected = 7;
    CHECK(!Shim::compareExchange(Shim::Word<std::uint16_t>{}, &obj, &expected, &desired));
    CHECK_EQ(obj, 9);
    CHECK_EQ(expected, 9);
    // the generic form, odd size
    std::array<std::uint8_t, 3> bytes{1, 2, 3};
    std::array<std::uint8_t, 3> want{1, 2, 3};
    std::array<std::uint8_t, 3> put{4, 5, 6};
    CHECK(Shim::compareExchange(Shim::Bytes{3}, bytes.data(), want.data(), put.data()));
    CHECK(bytes == put);
    CHECK(!Shim::compareExchange(Shim::Bytes{3}, bytes.data(), want.data(), put.data()));
    CHECK(want == put);
}

void loadStore() {
    Kvasir::Test::test("load and store");
    std::uint64_t volatile obj = 0;
    std::uint64_t const v      = 0x0102'0304'0506'0708ULL;
    Shim::store(Shim::Word<std::uint64_t>{}, &obj, &v);
    std::uint64_t got{};
    Shim::load(Shim::Word<std::uint64_t>{}, &obj, &got);
    CHECK_EQ(got, v);
    std::array<char, 5> src{'a', 'b', 'c', 'd', 'e'};
    std::array<char, 5> dst{};
    Shim::store(Shim::Bytes{5}, dst.data(), src.data());
    CHECK(dst == src);
    std::array<char, 5> back{};
    Shim::load(Shim::Bytes{5}, dst.data(), back.data());
    CHECK(back == src);
}

void fetchOps() {
    Kvasir::Test::test("fetchOp");
    std::uint8_t volatile b = 0xf0;
    CHECK_EQ(
      Shim::fetchOp<std::uint8_t>(&b, 0x20, [](std::uint8_t x, std::uint8_t y) { return x + y; }),
      0xf0);
    CHECK_EQ(b, 0x10);   // wraps in the operand's width
    std::uint32_t volatile w = 0b1100U;
    CHECK_EQ(
      Shim::fetchOp<std::uint32_t>(&w,
                                   0b1010U,
                                   [](std::uint32_t x, std::uint32_t y) { return ~(x & y); }),
      0b1100U);
    CHECK_EQ(w, ~0b1000U);
}
}   // namespace

int main() {
    exchangeWritesOnlyOnChange();
    exchangeWord();
    exchangeBytesWritesOnlyOnChange();
    compareExchange();
    loadStore();
    fetchOps();
    return Kvasir::Test::report();
}
