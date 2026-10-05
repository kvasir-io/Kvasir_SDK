// Kvasir::Flash::CellStore (Util/CellStore.hpp) on a fake NOR flash with power cuts (support/FakeNorFlash.hpp):
// round trips and sequences, SkipUnchanged; the ring's wear (erases even, no page programmed twice between erases,
// no 0 -> 1 asked for); a power cut at EVERY flash operation of a scenario, then a second cut in the recovery, for
// 2/3/4-sector stores with 1/2/16-page cells and a cell bigger than a sector - after each cut the store holds the
// last value stored or the one in flight, never an older one, never nothing; torn cells skipped and a sector of
// noise erased before use; the import of a SimpleEeprom record that never erases it before the copy exists (a cut
// at every operation of the import too); a version change with and without migrate.
#include "kvasir/Util/CellStore.hpp"
#include "support/FakeNorFlash.hpp"
#include "test_harness.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

namespace KF = Kvasir::Flash;

namespace {
template<std::size_t N>
struct Value {
    std::array<std::uint8_t, N> b{};

    bool operator==(Value const&) const = default;

    static Value make(std::uint32_t i) {
        Value v{};
        for(std::size_t j = 0; j != N; ++j) {
            v.b[j] = static_cast<std::uint8_t>(i * 31U + j * 7U + (i >> 3U));
        }
        return v;
    }
};

// a scenario of `count` distinct stores, the flash cut once after `cut` operations and once more `second` operations
// into the recovery; true when every check held
template<typename Flash,
         typename Store,
         typename V>
bool cutScenario(std::uint32_t count,
                 std::uint64_t cut,
                 std::uint64_t second,
                 bool&         cutHappened) {
    Flash::blank();
    Store::forget();
    Flash::cutAfter(cut);
    std::uint32_t lastOk = 0;
    std::uint32_t i      = 1;
    cutHappened          = false;
    for(; i <= count; ++i) {
        if(!Store::store(V::make(i))) {
            cutHappened = true;
            break;
        }
        lastOk = i;
    }
    if(!cutHappened) { return true; }
    auto const check = [&](std::uint32_t inFlight) {
        Flash::powerOn();
        Store::forget();
        V    got{};
        bool loaded = Store::load(got);
        if(lastOk == 0) { return !loaded || got == V::make(inFlight); }
        return loaded && (got == V::make(lastOk) || got == V::make(inFlight));
    };
    if(!check(i)) { return false; }
    // recovery: store the one that was cut and go on, cut again `second` operations in
    Flash::cutAfter(second);
    bool secondCut = false;
    for(; i <= count; ++i) {
        if(!Store::store(V::make(i))) {
            secondCut = true;
            break;
        }
        lastOk = i;
    }
    if(secondCut) {
        if(!check(i)) { return false; }
        for(; i <= count; ++i) {
            if(!Store::store(V::make(i))) { return false; }
        }
    }
    Store::forget();
    V got{};
    return Store::load(got) && got == V::make(count) && Flash::overprograms == 0;
}

template<typename Flash,
         typename Store,
         typename V>
void cutEverywhere(std::string_view what,
                   std::uint32_t    count) {
    Kvasir::Test::test(what);
    // the scenario's operations, without cuts
    Flash::blank();
    Store::forget();
    for(std::uint32_t i = 1; i <= count; ++i) { (void)Store::store(V::make(i)); }
    auto const total = Flash::ops;
    CHECK(total >= count);
    std::uint64_t cuts = 0;
    bool          ok   = true;
    for(std::uint64_t k = 0; k <= total && ok; ++k) {
        bool cutHappened = false;
        ok               = cutScenario<Flash, Store, V>(count, k, (k * 7U) % 5U, cutHappened);
        if(cutHappened) { ++cuts; }
        if(!ok) { std::print("  cut after {} operations broke the store\n", k); }
    }
    CHECK(ok);
    CHECK(cuts == total);
}

struct Rp2;
struct Rp3;
struct Rp4;
struct Rp4b;
struct Rp4c;
struct Big;
struct Wear;
struct Torn;
struct Leg;
struct Ver;

using V8    = Value<8>;
using V300  = Value<300>;
using V4000 = Value<4000>;
using V6000 = Value<6000>;

using Flash2   = Kvasir::Test::FakeNorFlash<Rp2, 4096, 256, 2>;
using Flash3   = Kvasir::Test::FakeNorFlash<Rp3, 4096, 256, 3>;
using Flash4   = Kvasir::Test::FakeNorFlash<Rp4, 4096, 256, 4>;
using Flash4b  = Kvasir::Test::FakeNorFlash<Rp4b, 4096, 256, 4>;
using Flash4c  = Kvasir::Test::FakeNorFlash<Rp4c, 4096, 256, 2>;
using FlashBig = Kvasir::Test::FakeNorFlash<Big, 4096, 256, 4>;
using FlashW   = Kvasir::Test::FakeNorFlash<Wear, 4096, 256, 2>;
using FlashT   = Kvasir::Test::FakeNorFlash<Torn, 4096, 256, 2>;
using FlashL   = Kvasir::Test::FakeNorFlash<Leg, 4096, 256, 2>;
using FlashV   = Kvasir::Test::FakeNorFlash<Ver, 4096, 256, 2>;
// the SAM shape: 256-byte rows of 64-byte pages
struct Sam;
using FlashSam = Kvasir::Test::FakeNorFlash<Sam, 256, 64, 4>;

// SimpleEeprom's record {T, uint16 crc} with a CRC-16, read from the fake flash at sector 1
struct LegacyRecord {
    V8            v;
    std::uint16_t crc;
};

std::uint16_t crc16(V8 const& v) {
    return static_cast<std::uint16_t>(
      Kvasir::Crc::Crc16Ccitt<>::compute(std::as_bytes(std::span{&v, 1})));
}

struct TestLegacy {
    static bool read(V8& out) {
        LegacyRecord r{};
        std::memcpy(&r, FlashL::raw(4096, sizeof r).data(), sizeof r);
        if(r.crc != crc16(r.v)) { return false; }
        out = r.v;
        return true;
    }

    template<typename Region>
    static constexpr std::optional<std::uint32_t> sectorIn() {
        return 1;
    }
};

struct LegacyCfg : KF::CellStoreDefaults {
    using Legacy = TestLegacy;
};

struct V1 {
    std::uint32_t a;
};

struct V2 {
    std::uint32_t a;
    std::uint32_t b;
};

struct V2Cfg : KF::CellStoreDefaults {
    static constexpr std::uint16_t Version = 2;
};

struct V2Migrate : V2Cfg {
    static bool migrate(std::uint16_t version,
                        std::uint16_t length,
                        auto const&   read,
                        V2&           out) {
        V1 old{};
        if(version != 1 || length != sizeof old
           || !read(0, std::as_writable_bytes(std::span{&old, 1})))
        {
            return false;
        }
        out = V2{old.a, 77};
        return true;
    }
};

struct EraseAheadCfg : KF::CellStoreDefaults {
    static constexpr std::size_t EraseAhead = 2;
};
}   // namespace

int main() {
    using Kvasir::Test::test;

    {
        test("basics: empty, round trip, sequences 1 2 3 in cells 0 1 2, a reboot, SkipUnchanged");
        using S = KF::CellStore<Flash2, V8>;
        static_assert(S::CellBytes == 256 && S::CellsPerBlock == 16 && S::Blocks == 2);
        Flash2::blank();
        S::forget();
        V8 got{};
        CHECK(!S::load(got) && S::stats().sequence == 0);
        for(std::uint32_t i = 1; i <= 3; ++i) {
            auto const w = S::store(V8::make(i));
            CHECK(w && w->sequence == i && w->block == 0 && w->cell == i - 1 && !w->erased
                  && !w->unchanged);
        }
        S::forget();
        CHECK(S::load(got) && got == V8::make(3) && S::stats().sequence == 3
              && S::stats().scanned == 32);
        auto const before = Flash2::ops;
        auto const same   = S::store(V8::make(3));
        CHECK(same && same->unchanged && Flash2::ops == before);
        CHECK(S::load() == V8::make(3));
    }

    {
        test("ring and wear: 100 000 stores into 2 x 16 cells");
        using S = KF::CellStore<FlashW, V8>;
        FlashW::blank();
        S::forget();
        std::uint32_t erasedStores = 0;
        bool          allOk        = true;
        for(std::uint32_t i = 1; i <= 100'000; ++i) {
            auto const w = S::store(V8::make(i));
            allOk        = allOk && w.has_value();
            if(w && w->erased) { ++erasedStores; }
        }
        // the first round over both blank sectors erases nothing: (100 000 - 32) / 16 erases
        constexpr std::uint32_t Erases = (100'000 - 32) / 16;
        CHECK(allOk && erasedStores == Erases);
        auto const e0 = FlashW::erases[0];
        auto const e1 = FlashW::erases[1];
        CHECK((e0 > e1 ? e0 - e1 : e1 - e0) <= 1);
        CHECK(e0 + e1 == Erases);
        CHECK(FlashW::maxPagePrograms() <= 1 && FlashW::overprograms == 0
              && FlashW::pageCrossings == 0);
        S::forget();
        CHECK(S::load() == V8::make(100'000));
    }

    cutEverywhere<Flash2, KF::CellStore<Flash2, V8>, V8>(
      "power cut at every operation: 2 sectors, 1-page cells",
      60);
    cutEverywhere<Flash3, KF::CellStore<Flash3, V8>, V8>(
      "power cut at every operation: 3 sectors, 1-page cells",
      60);
    cutEverywhere<Flash4, KF::CellStore<Flash4, V300>, V300>(
      "power cut at every operation: 4 sectors, 2-page cells",
      40);
    cutEverywhere<Flash4b, KF::CellStore<Flash4b, V4000>, V4000>(
      "power cut at every operation: 4 sectors, 16-page cells",
      12);
    cutEverywhere<Flash4c, KF::CellStore<Flash4c, V300>, V300>(
      "power cut at every operation: 2 sectors, 2-page cells",
      30);
    {
        using S = KF::CellStore<FlashBig, V6000>;
        static_assert(S::CellBytes == 8192 && S::SectorsPerBlock == 2 && S::Blocks == 2
                      && S::CellsPerBlock == 1);
        cutEverywhere<FlashBig, S, V6000>(
          "power cut at every operation: a cell of two sectors (omniscope's shape)",
          6);
    }
    {
        using S = KF::CellStore<FlashSam, V8>;
        static_assert(S::CellBytes == 64 && S::CellsPerBlock == 4 && S::Blocks == 4);
        cutEverywhere<FlashSam, S, V8>(
          "power cut at every operation: the SAM shape (64-byte pages, 256-byte rows)",
          40);
    }

    {
        test("torn cells: skipped and counted; a sector of noise is erased before use");
        using S = KF::CellStore<FlashT, V8>;
        FlashT::blank();
        S::forget();
        for(std::uint32_t i = 1; i <= 4; ++i) { (void)S::store(V8::make(i)); }
        auto torn = FlashT::raw(5 * 256, 256);   // cell 5: noise
        for(std::size_t i = 0; i != torn.size(); ++i) {
            torn[i] = static_cast<std::byte>(i * 13U + 1U);
        }
        for(std::size_t i = 0; i != 4096; ++i) {   // sector 1: noise
            FlashT::raw(4096, 4096)[i] = static_cast<std::byte>(i * 29U + 3U);
        }
        S::forget();
        CHECK(S::load() == V8::make(4) && S::stats().torn == 17);
        auto const w5 = S::store(V8::make(5));   // cell 4
        auto const w6 = S::store(V8::make(6));   // cell 5 is torn: cell 6
        CHECK(w5 && w5->cell == 4 && w6 && w6->cell == 6);
        for(std::uint32_t i = 7; i <= 15; ++i) {
            (void)S::store(V8::make(i));
        }   // cells 7..15: sector 0 full
        auto const into1 = S::store(V8::make(16));
        CHECK(into1 && into1->block == 1 && into1->cell == 0 && into1->erased);
        S::forget();
        CHECK(S::load() == V8::make(16) && FlashT::overprograms == 0);
    }

    {
        test("legacy import: into the other sector, never over the record before the copy exists");
        using S        = KF::CellStore<FlashL, V8, LegacyCfg>;
        auto const put = [] {
            FlashL::blank();
            LegacyRecord r{V8::make(42), crc16(V8::make(42))};
            std::memcpy(FlashL::raw(4096, sizeof r).data(), &r, sizeof r);
        };
        put();
        S::forget();
        V8 got{};
        CHECK(S::load(got) && got == V8::make(42) && S::stats().imported && S::stats().block == 0);
        CHECK(FlashL::raw(4096, 1)[0] != std::byte{0xFF});   // the legacy bytes untouched
        // a cut at every operation of the import: the import or the legacy record is there
        bool everyCut = true;
        for(std::uint64_t k = 0; k != 4; ++k) {
            put();
            S::forget();
            FlashL::cutAfter(k);
            V8 v{};
            (void)S::load(v);
            FlashL::powerOn();
            S::forget();
            V8 again{};
            everyCut = everyCut && S::load(again) && again == V8::make(42);
        }
        CHECK(everyCut);
        // a legacy record with a bad CRC: nothing
        FlashL::blank();
        LegacyRecord bad{V8::make(42), static_cast<std::uint16_t>(crc16(V8::make(42)) ^ 1U)};
        std::memcpy(FlashL::raw(4096, sizeof bad).data(), &bad, sizeof bad);
        S::forget();
        CHECK(!S::load(got) && !S::stats().imported);
    }

    {
        test(
          "versions: a migrate reads the old cell; without it nothing, and the next store goes "
          "after it");
        using S1 = KF::CellStore<FlashV, V1>;
        FlashV::blank();
        S1::forget();
        (void)S1::store(V1{5});
        using S2m = KF::CellStore<FlashV, V2, V2Migrate>;
        S2m::forget();
        V2 got{};
        CHECK(S2m::load(got) && got.a == 5 && got.b == 77);
        using S2 = KF::CellStore<FlashV, V2, V2Cfg>;
        S2::forget();
        CHECK(!S2::load(got));
        auto const w = S2::store(V2{6, 7});
        CHECK(w && w->cell == 1 && w->sequence == 2);
        S2::forget();
        CHECK(S2::load(got) && got.a == 6 && got.b == 7);
    }

    {
        test(
          "CRC: a flipped payload bit makes the newest cell invalid, the one before is newest "
          "again");
        using S = KF::CellStore<Flash2, V8>;
        Flash2::blank();
        S::forget();
        (void)S::store(V8::make(1));
        (void)S::store(V8::make(2));
        Flash2::raw(256 + 16, 1)[0] ^= std::byte{0x04};
        S::forget();
        CHECK(S::load() == V8::make(1) && S::stats().invalid == 1);
    }

    {
        test("EraseAhead: maintain() erases the next sector early, the store then needs no erase");
        using S = KF::CellStore<Flash2, V8, EraseAheadCfg>;
        Flash2::blank();
        S::forget();
        for(std::uint32_t i = 1; i <= 16; ++i) { (void)S::store(V8::make(i)); }    // sector 0 full
        for(std::uint32_t i = 17; i <= 32; ++i) { (void)S::store(V8::make(i)); }   // sector 1 full
        for(std::uint32_t i = 33; i <= 46; ++i) {
            (void)S::store(V8::make(i));
        }   // sector 0 again, 2 left
        auto const erases = Flash2::erases[1];
        S::maintain();
        CHECK(Flash2::erases[1] == erases + 1);
        S::maintain();   // once only
        CHECK(Flash2::erases[1] == erases + 1);
        (void)S::store(V8::make(47));
        (void)S::store(V8::make(48));
        auto const w = S::store(V8::make(49));
        CHECK(w && w->block == 1 && w->cell == 0 && !w->erased);
        static_assert(!std::is_same_v<S::Extends, brigand::list<>>);
        static_assert(std::is_same_v<KF::CellStore<Flash2, V8>::Extends, brigand::list<>>);
    }

    return Kvasir::Test::report();
}
