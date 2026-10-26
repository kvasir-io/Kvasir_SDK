// Kvasir::Crc::ImageEngine and its hook ImageEngines<Tag::User> (Runtime/CrcImageEngine.hpp): the image's set is taken where a user
// is instantiated (the specialisation below comes after the library headers), a CRC the set leaves to the software
// stays there, a constant expression is always the software, and the libraries' defaults follow the set - a
// CellStore's cells are written and checked by the image's engine with the same CRC as the software. The image's
// engine here is a stand-in for a chip's: the software, counted, and made to fail as a chip's hardware can (a DMA
// transfer that times out or hits a bus error): ImageEngine::failed() says so, compute() still gives the CRC, and a
// CellStore's store writes nothing (CellError::crc), its mount checks the cell again with the software.
#include "kvasir/Runtime/CellStore.hpp"
#include "kvasir/Runtime/CrcImageEngine.hpp"
#include "kvasir/Util/Crc.hpp"
#include "support/FakeNorFlash.hpp"
#include "test_harness.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace {
std::uint32_t engines = 0;   // the stand-in engines made: one a CRC
bool failing = false;        // every update() of a stand-in engine fails, as a chip's hardware can

template<auto P>
class CountedEngine {
public:
    using value_type = typename Kvasir::Crc::Engine<P>::value_type;

    CountedEngine() { ++engines; }

    CountedEngine& update(std::span<std::byte const> data) {
        if(failing) { failed_ = true; }
        if(!failed_) { e_.update(data); }
        return *this;
    }

    [[nodiscard]] bool failed() const { return failed_; }

    [[nodiscard]] value_type finish() const { return e_.finish(); }

    // the contract of a set's engine: compute() is the CRC whatever happens (the software where the hardware fails)
    [[nodiscard]] static value_type compute(std::span<std::byte const> data) {
        CountedEngine e{};
        e.update(data);
        return e.failed() ? Kvasir::Crc::Engine<P, 0>::compute(data) : e.finish();
    }

private:
    Kvasir::Crc::Engine<P, 0> e_{};
    bool                      failed_{};
};

// the shape of a chip's set: its engine for the CRC-32s, the software for the rest
struct CountedSet {
    template<auto P, std::size_t TableSize>
    using For
      = std::conditional_t<P.width == 32, CountedEngine<P>, Kvasir::Crc::Engine<P, TableSize>>;
};
}   // namespace

template<>
struct Kvasir::Crc::ImageEngines<Kvasir::Tag::User> {
    using type = CountedSet;
};

namespace {
namespace Crc = Kvasir::Crc;
namespace P   = Kvasir::Crc::Presets;

static_assert(std::is_same_v<Crc::ImageEngine<P::crc32IsoHdlc>::Selected,
                             CountedEngine<P::crc32IsoHdlc>>);
static_assert(std::is_same_v<Crc::ImageEngine<P::crc16Xmodem,
                                              256>::Selected,
                             Crc::Engine<P::crc16Xmodem,
                                         256>>,
              "a CRC the set leaves to the software keeps the caller's table size");
static_assert(std::is_same_v<Kvasir::Flash::CellStoreDefaults::Crc,
                             Crc::ImageEngine<P::crc32IsoHdlc>>);

constexpr std::array<std::byte, 9> checkBytes{std::byte{'1'},
                                              std::byte{'2'},
                                              std::byte{'3'},
                                              std::byte{'4'},
                                              std::byte{'5'},
                                              std::byte{'6'},
                                              std::byte{'7'},
                                              std::byte{'8'},
                                              std::byte{'9'}};
// a constant expression is the software (the stand-in's constructor is not constexpr)
static_assert(Crc::ImageEngine<P::crc32IsoHdlc>::compute(checkBytes) == P::crc32IsoHdlc.check);

struct Region;
using Flash = Kvasir::Test::FakeNorFlash<Region, 4096, 256, 2>;

struct Value {
    std::array<std::uint8_t, 40> b{};

    bool operator==(Value const&) const = default;
};

using Store = Kvasir::Flash::CellStore<Flash, Value>;
}   // namespace

int main() {
    using Kvasir::Test::test;
    // not constexpr: a constant input makes the call a constant expression wherever it initialises a const integer
    // (CHECK_EQ's locals) - `if consteval` is true there, and the software runs
    auto bytes = checkBytes;

    test("the image's engine at run time, the software for the rest");
    engines = 0;
    CHECK_EQ(Crc::ImageEngine<P::crc32IsoHdlc>::compute(bytes), P::crc32IsoHdlc.check);
    CHECK_EQ(engines, 1U);
    using Xmodem = Crc::ImageEngine<P::crc16Xmodem, 256>;
    CHECK_EQ(Xmodem::compute(bytes), P::crc16Xmodem.check);
    CHECK_EQ(engines, 1U);
    CHECK_EQ(Crc::ImageEngine<P::crc32IsoHdlc>::compute(checkBytes), P::crc32IsoHdlc.check);
    CHECK_EQ(engines, 1U);

    test("incremental, the same CRC as one pass");
    Crc::ImageEngine<P::crc32IsoHdlc> e;
    e.update(std::span{bytes}.first(4)).update(std::span{bytes}.subspan(4));
    CHECK_EQ(e.finish(), P::crc32IsoHdlc.check);

    test("a CellStore's cells on the image's engine, the software's CRC in the cell");
    Flash::blank();
    Store::forget();
    Value v{};
    for(std::size_t i = 0; i < v.b.size(); ++i) { v.b[i] = static_cast<std::uint8_t>(i * 7U + 1U); }
    engines            = 0;
    auto const written = Store::store(v);
    CHECK(written.has_value());
    CHECK(engines != 0U);
    std::array<std::byte, 16 + sizeof(Value)> cell{};
    CHECK(Flash::read(0, cell));
    std::uint32_t const stored = std::to_integer<std::uint32_t>(cell[12])
                               | std::to_integer<std::uint32_t>(cell[13]) << 8U
                               | std::to_integer<std::uint32_t>(cell[14]) << 16U
                               | std::to_integer<std::uint32_t>(cell[15]) << 24U;
    CHECK_EQ(stored,
             Crc::Crc32{}
               .update(std::span<std::byte const>{cell}.first(12))
               .update(std::span<std::byte const>{cell}.subspan(16))
               .finish());
    Store::forget();
    Value back{};
    CHECK(Store::load(back));
    CHECK(back == v);

    test(
      "a failing engine: failed() says so, compute() is still the CRC, the software never fails");
    failing = true;
    {
        Crc::ImageEngine<P::crc32IsoHdlc> f;
        f.update(std::span{bytes});
        CHECK(f.failed());
        CHECK_EQ(Crc::ImageEngine<P::crc32IsoHdlc>::compute(bytes), P::crc32IsoHdlc.check);
        Xmodem x;
        x.update(std::span{bytes});
        CHECK(!x.failed());
        CHECK_EQ(x.finish(), P::crc16Xmodem.check);
    }

    test(
      "a CellStore on a failing engine: a store writes nothing, a mount checks the cells with the "
      "software");
    {
        Value w              = v;
        w.b[0]               = 0xA5;
        auto const opsBefore = Flash::ops;
        auto const refused   = Store::store(w);
        CHECK(!refused && refused.error() == Kvasir::Flash::CellError::crc);
        CHECK_EQ(Flash::ops, opsBefore);
        Store::forget();
        // the one cell checked again by the software: loaded, not skipped as unreadable - skipped, the store after the
        // engine came back would start over at sequence 1 and erase the block that holds the newest cell
        Value got{};
        CHECK(Store::load(got) && got == v);
        CHECK_EQ(Store::stats().invalid, 0U);
        failing          = false;
        auto const again = Store::store(w);
        CHECK(again.has_value() && written.has_value() && again->sequence == written->sequence + 1
              && !again->erased);
        Store::forget();
        CHECK(Store::load(back) && back == w);
    }

    return Kvasir::Test::report();
}
