// Kvasir::Crc::Engine: every preset's check value at every table size, parameter sets that exercise
// the corners (a width below a byte's, refin != refout, 64 bits), the table sizes against each other on
// random data, the incremental and resumed forms, and the vectors the code it replaced asserted.
// Most of it is checked at compile time.
#include "kvasir/Util/Crc.hpp"
#include "test_harness.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <string_view>
#include <vector>

namespace Crc = Kvasir::Crc;
namespace P   = Kvasir::Crc::Presets;

namespace {

template<auto Params>
constexpr bool checkAllSizes() {
    constexpr std::string_view in = "123456789";
    return Crc::Engine<Params, 0>::compute(in) == Params.check
        && Crc::Engine<Params, 4>::compute(in) == Params.check
        && Crc::Engine<Params, 16>::compute(in) == Params.check
        && Crc::Engine<Params, 256>::compute(in) == Params.check;
}

static_assert(checkAllSizes<P::crc8Smbus>());
static_assert(checkAllSizes<P::crc8MaximDow>());
static_assert(checkAllSizes<P::crc8Nrsc5>());
static_assert(checkAllSizes<P::crc8Htu21d>());
static_assert(checkAllSizes<P::crc7Mmc>());
static_assert(checkAllSizes<P::crc16Xmodem>());
static_assert(checkAllSizes<P::crc16Ibm3740>());
static_assert(checkAllSizes<P::crc16Kermit>());
static_assert(checkAllSizes<P::crc16X25>());
static_assert(checkAllSizes<P::crc32IsoHdlc>());
static_assert(checkAllSizes<P::crc32Bzip2>());
static_assert(checkAllSizes<P::crc32Mpeg2>());
static_assert(checkAllSizes<P::crc32c>());

// Corners no preset covers: a width of 5 with an xorout, input and output reflected differently,
// and a 64-bit value type.
constexpr Crc::Params<std::uint8_t>  crc5Usb{5, 0x05, 0x1F, true, true, 0x1F, 0x19};
constexpr Crc::Params<std::uint16_t> crc12Umts{12, 0x80F, 0x000, false, true, 0x000, 0xDAF};
constexpr Crc::Params<std::uint64_t>
  crc64Xz{64, 0x42F0E1EBA9EA3693ULL, ~0ULL, true, true, ~0ULL, 0x995DC9BBDF1939FAULL};
static_assert(checkAllSizes<crc5Usb>());
static_assert(checkAllSizes<crc12Umts>());
static_assert(checkAllSizes<crc64Xz>());

// The default table sizes and what they cost.
static_assert(Crc::Crc32::tableSize == 16 && sizeof(Crc::Crc32::table) == 64);
static_assert(Crc::Crc8Sensirion<>::tableSize == 0 && Crc::Crc7Mmc<>::tableSize == 0);
static_assert(Crc::Crc16Ccitt<>::tableSize == 16 && sizeof(Crc::Crc16Ccitt<>::table) == 32);

// Incremental: any split gives the one-shot result, and finish() leaves the state alone.
constexpr bool splitsAgree() {
    constexpr std::string_view in = "123456789";
    for(std::size_t cut = 0; cut <= in.size(); ++cut) {
        Crc::Crc32 e;
        e.update(in.substr(0, cut));
        auto const mid = e.finish();
        static_cast<void>(mid);
        e.update(in.substr(cut));
        if(e.finish() != P::crc32IsoHdlc.check || e.finish() != e.finish()) { return false; }
    }
    return true;
}

static_assert(splitsAgree());

// resume(compute(a)).update(b) == compute(a + b), zlib's chaining, where refin == refout.
template<typename E>
constexpr bool resumes() {
    constexpr std::string_view a = "12345";
    constexpr std::string_view b = "6789";
    return E::resume(E::compute(a)).update(b).finish() == E::params.check
        && E::resume(E::compute(std::string_view{})).update("123456789").finish()
             == E::params.check;
}

static_assert(resumes<Crc::Crc32>());
static_assert(resumes<Crc::Crc32T<0>>());
static_assert(resumes<Crc::Crc16Ccitt<>>());
static_assert(resumes<Crc::Crc16Xmodem<256>>());
static_assert(resumes<Crc::Crc8MaximDow<>>());
static_assert(resumes<Crc::Crc7Mmc<>>());

// The vectors of the code the engine replaces, so a migration that loses one fails here too.
template<typename E,
         std::size_t N>
constexpr auto crcOf(std::array<std::uint8_t,
                                N> const& bytes) {
    return E::compute(std::span<std::uint8_t const>{bytes});
}

// kvasir_devices Bytes.hpp: Sensirion CRC(0xBEEF) = 0x92 (SHT3x Table 20)
static_assert(crcOf<Crc::Crc8Sensirion<>>(std::array<std::uint8_t,
                                                     2>{0xBE,
                                                        0xEF})
              == 0x92);
// Bytes.hpp: Maxim AN27's example ROM 02 1C B8 01 00 00 00 -> 0xA2; a ROM with its CRC -> 0
static_assert(crcOf<Crc::Crc8MaximDow<>>(std::array<std::uint8_t,
                                                    7>{0x02,
                                                       0x1C,
                                                       0xB8,
                                                       0x01,
                                                       0,
                                                       0,
                                                       0})
              == 0xA2);
static_assert(crcOf<Crc::Crc8MaximDow<>>(std::array<std::uint8_t,
                                                    8>{0x02,
                                                       0x1C,
                                                       0xB8,
                                                       0x01,
                                                       0,
                                                       0,
                                                       0,
                                                       0xA2})
              == 0x00);
// Htu21d.hpp: CRC(0x683A) = 0x7C, CRC(0x4E85) = 0x6B
static_assert(crcOf<Crc::Crc8Htu21d<>>(std::array<std::uint8_t,
                                                  2>{0x68,
                                                     0x3A})
              == 0x7C);
static_assert(crcOf<Crc::Crc8Htu21d<>>(std::array<std::uint8_t,
                                                  2>{0x4E,
                                                     0x85})
              == 0x6B);
// SdCard.hpp: CMD0's CRC byte 0x95, CMD8(0x1AA)'s 0x87 (the CRC-7 shifted up, end bit set)
static_assert(((crcOf<Crc::Crc7Mmc<>>(std::array<std::uint8_t,
                                                 5>{0x40,
                                                    0,
                                                    0,
                                                    0,
                                                    0})
                << 1)
               | 1)
              == 0x95);
static_assert(((crcOf<Crc::Crc7Mmc<>>(std::array<std::uint8_t,
                                                 5>{0x48,
                                                    0,
                                                    0,
                                                    0x01,
                                                    0xAA})
                << 1)
               | 1)
              == 0x87);

// SdCard.hpp: CRC16 of a 512-byte block of 0xFF is 0x7FA1, of 0x00 is 0
template<std::uint8_t V>
constexpr std::uint16_t sdBlock() {
    std::array<std::uint8_t, 512> b{};
    for(auto& x : b) { x = V; }
    return Crc::Crc16Xmodem<256>::compute(std::span<std::uint8_t const>{b});
}

static_assert(sdBlock<0xFF>() == 0x7FA1 && sdBlock<0x00>() == 0x0000);

// The Calc adapter: SimpleEeprom's and aglio's `type` + `calc(span)`.
using EepromCrc = Crc::Calc<Crc::Crc16Ccitt<0>>;
static_assert(std::is_same_v<EepromCrc::type,
                             std::uint16_t>);

// a bitwise CRC-32/ISO-HDLC written out the textbook way, as an independent reference
std::uint32_t referenceCrc32(std::span<std::uint8_t const> data) {
    std::uint32_t crc = 0xFFFF'FFFFU;
    for(auto const b : data) {
        crc ^= b;
        for(int i = 0; i < 8; ++i) { crc = (crc >> 1) ^ ((crc & 1U) != 0 ? 0xEDB8'8320U : 0U); }
    }
    return ~crc;
}

template<auto Params>
bool sizesAgree(std::span<std::uint8_t const> data) {
    auto const a = Crc::Engine<Params, 0>::compute(data);
    return a == Crc::Engine<Params, 4>::compute(data) && a == Crc::Engine<Params, 16>::compute(data)
        && a == Crc::Engine<Params, 256>::compute(data);
}

}   // namespace

int main() {
    using Kvasir::Test::test;

    std::mt19937              rng{20261003};
    std::vector<std::uint8_t> data(64 * 1024);
    for(auto& b : data) { b = static_cast<std::uint8_t>(rng()); }
    std::span<std::uint8_t const> const all{data};

    test("table sizes agree on 64 KiB of random bytes");
    CHECK(sizesAgree<P::crc8Smbus>(all));
    CHECK(sizesAgree<P::crc8MaximDow>(all));
    CHECK(sizesAgree<P::crc7Mmc>(all));
    CHECK(sizesAgree<P::crc16Xmodem>(all));
    CHECK(sizesAgree<P::crc16X25>(all));
    CHECK(sizesAgree<P::crc32IsoHdlc>(all));
    CHECK(sizesAgree<P::crc32Bzip2>(all));
    CHECK(sizesAgree<crc12Umts>(all));
    CHECK(sizesAgree<crc64Xz>(all));

    test("CRC-32 against a textbook bitwise loop");
    CHECK_EQ(Crc::Crc32::compute(all), referenceCrc32(all));
    CHECK_EQ(Crc::Crc32T<256>::compute(all.first(1000)), referenceCrc32(all.first(1000)));

    test("the Calc adapter");
    CHECK_EQ(EepromCrc::calc(std::as_bytes(std::span{std::string_view{"123456789"}})), 0x29B1U);

    test("incremental and resumed at run time");
    for(std::size_t cut : {std::size_t{0}, std::size_t{1}, std::size_t{4095}, data.size()}) {
        Crc::Crc32 e;
        e.update(all.first(cut)).update(all.subspan(cut));
        CHECK_EQ(e.finish(), referenceCrc32(all));
        CHECK_EQ(
          Crc::Crc32::resume(Crc::Crc32::compute(all.first(cut))).update(all.subspan(cut)).finish(),
          referenceCrc32(all));
    }

    return Kvasir::Test::report();
}
