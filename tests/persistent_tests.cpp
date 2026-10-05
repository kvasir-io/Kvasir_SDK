// Kvasir::Persistent<T, Tag>: a value in .noInit trusted only by its header and CRC. Garbage, every
// single flipped bit, another build's header and a reset in the middle of a store all read as "nothing";
// store/load/take/clear/update do what they say; the layout and CRC are what the host tools decode.
#include "kvasir/Util/Persistent.hpp"

#include "test_harness.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace {

struct Counter {
    std::uint32_t failures;
    std::uint32_t lastCause;
    std::uint32_t pc;
};

struct CounterTag {
    static constexpr std::uint32_t id      = Kvasir::persistentId("KBTG");
    static constexpr std::uint16_t version = 1;
};

struct OtherVersionTag {
    static constexpr std::uint32_t id      = Kvasir::persistentId("KBTG");
    static constexpr std::uint16_t version = 2;
};

using P = Kvasir::Persistent<Counter, CounterTag>;
static_assert(P::words == 2 + 3 + 1);
static_assert(sizeof(P) == P::words * 4,
              "nothing but the words: host tools read it by its symbol's size");
static_assert(Kvasir::persistentId("KFLT") == 0x544C'464BU,
              "'K' 'F' 'L' 'T' in memory order");

// The object's words, as a host tool or a reset sees them.
std::uint32_t volatile* wordsOf(P& p) { return reinterpret_cast<std::uint32_t volatile*>(&p); }

void fill(P&            p,
          std::uint32_t v) {
    for(std::size_t i = 0; i != P::words; ++i) { wordsOf(p)[i] = v; }
}

std::array<std::uint32_t,
           P::words>
snapshot(P& p) {
    std::array<std::uint32_t, P::words> r{};
    for(std::size_t i = 0; i != P::words; ++i) { r[i] = wordsOf(p)[i]; }
    return r;
}

void restore(P&                          p,
             std::array<std::uint32_t,
                        P::words> const& r) {
    for(std::size_t i = 0; i != P::words; ++i) { wordsOf(p)[i] = r[i]; }
}

bool same(std::optional<Counter> const& a,
          Counter const&                b) {
    return a && a->failures == b.failures && a->lastCause == b.lastCause && a->pc == b.pc;
}

}   // namespace

int main() {
    using Kvasir::Test::test;

    // A static object, as in firmware; host statics are zeroed, so make it look like RAM after a reset.
    static P p;

    test("garbage loads nothing");
    fill(p, 0xA5A5'A5A5U);
    CHECK(!p.load());
    CHECK(!p.valid());
    fill(p, 0);
    CHECK(!p.load());

    test("store, load, take, clear");
    Counter const c{.failures = 3, .lastCause = 7, .pc = 0x1000'3A5EU};
    p.store(c);
    CHECK(same(p.load(), c));
    CHECK(p.valid());
    CHECK(same(p.take(), c));
    CHECK(!p.load());
    CHECK(!p.take());
    p.store(c);
    p.clear();
    CHECK(!p.load());
    p.clear();   // twice: the CRC flips back, the id is still gone
    CHECK(!p.load());

    test("update starts from T{} or from the value");
    fill(p, 0xFFFF'FFFFU);
    p.update([](Counter& v) { ++v.failures; });
    CHECK(p.load() && p.load()->failures == 1 && p.load()->pc == 0);
    p.update([](Counter& v) { ++v.failures; });
    CHECK(p.load() && p.load()->failures == 2);

    test("layout and CRC are what the host tools decode");
    p.store(c);
    auto const r = snapshot(p);
    CHECK_EQ(r[0], CounterTag::id);
    CHECK_EQ(r[1], 1U | (12U << 16));
    CHECK_EQ(r[2], 3U);
    CHECK_EQ(r[3], 7U);
    CHECK_EQ(r[4], 0x1000'3A5EU);
    {
        // zlib's crc32 over words 0..4 as little-endian bytes
        std::array<std::byte, 20> bytes{};
        for(std::size_t i = 0; i != 5; ++i) {
            for(std::size_t b = 0; b != 4; ++b) {
                bytes[i * 4 + b] = static_cast<std::byte>(r[i] >> (8 * b));
            }
        }
        CHECK_EQ(r[5], Kvasir::Crc::Crc32::compute(bytes));
    }

    test("every single flipped bit reads as nothing");
    for(std::size_t w = 0; w != P::words; ++w) {
        for(unsigned bit = 0; bit != 32; ++bit) {
            auto bad = r;
            bad[w] ^= 1U << bit;
            restore(p, bad);
            CHECK(!p.load());
        }
    }

    test("another build's header reads as nothing");
    {
        static Kvasir::Persistent<Counter, OtherVersionTag> other;
        other.store(c);
        restore(p, [&] {
            std::array<std::uint32_t, P::words> o{};
            for(std::size_t i = 0; i != P::words; ++i) {
                o[i] = reinterpret_cast<std::uint32_t volatile*>(&other)[i];
            }
            return o;
        }());
        CHECK(!p.load());

        struct Bigger {
            std::uint32_t a, b, c, d;
        };

        static Kvasir::Persistent<Bigger, CounterTag> bigger;
        bigger.store(Bigger{3, 7, 0x1000'3A5EU, 0});
        restore(p, [&] {
            std::array<std::uint32_t, P::words> o{};
            for(std::size_t i = 0; i != P::words; ++i) {
                o[i] = reinterpret_cast<std::uint32_t volatile*>(&bigger)[i];
            }
            return o;
        }());
        CHECK(!p.load());
    }

    test("a reset after any number of a store's word writes: the old value or nothing");
    {
        Counter const oldValue{.failures = 1, .lastCause = 2, .pc = 3};
        Counter const newValue{.failures = 9, .lastCause = 8, .pc = 7};
        p.store(oldValue);
        auto const before = snapshot(p);
        p.store(newValue);
        auto const after = snapshot(p);
        // store()'s order: word 0 cleared, words 1 .. n-1, then word 0 (the id)
        std::array<std::size_t, P::words + 1> order{};
        order[0] = 0;
        for(std::size_t i = 1; i != P::words; ++i) { order[i] = i; }
        order[P::words] = 0;
        for(std::size_t k = 0; k <= P::words + 1; ++k) {
            auto state = before;
            for(std::size_t i = 0; i != k && i != order.size(); ++i) {
                state[order[i]] = i == 0 ? 0U : after[order[i]];
            }
            restore(p, state);
            auto const got = p.load();
            if(k == 0) {
                CHECK(same(got, oldValue));
            } else if(k >= order.size()) {
                CHECK(same(got, newValue));
            } else {
                CHECK(!got);
            }
        }
    }

    return Kvasir::Test::report();
}
