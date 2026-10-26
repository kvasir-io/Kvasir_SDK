#pragma once
// One CRC engine for every CRC in Kvasir: the Rocksoft model (width, poly, init, refin, refout,
// xorout) with the catalogue's check value - the CRC of the ASCII bytes "123456789" - as a
// compile-time self-test, a table chosen per use (none / 4 / 16 / 256 entries), constexpr
// throughout, an incremental update() / finish() and a one-shot compute().
//
//   auto const c = Kvasir::Crc::Crc32::compute(std::as_bytes(std::span{record}));
//   Kvasir::Crc::Crc32 e;  e.update(header).update(payload);  auto const crc = e.finish();
//   auto const more = Kvasir::Crc::Crc32::resume(previous).update(tail).finish();   // zlib's crc32(prev, tail)
//
// A CRC the catalogue does not name is one Params value; its check value is part of it, so a
// wrong poly, init or reflection is a compile error at the first finish():
//
//   inline constexpr Kvasir::Crc::Params<std::uint8_t> MyCrc{
//     .width = 8, .poly = 0x31, .init = 0x00, .refin = false, .refout = false, .xorout = 0, .check = 0xA2};
//   using MyEngine = Kvasir::Crc::Engine<MyCrc, 0>;
//
// Table sizes: 0 = bitwise (no table), 4, 16, 256 entries of .rodata, one table per (Params, size)
// used at run time. Defaults: bitwise for 8-bit and smaller CRCs, a nibble table for 16/32-bit
// ones (64 bytes for CRC-32). Say <256> where the data rate matters.
//
// The chip packages' Kvasir::DmaCrc namespace holds the hardware engines, which take the same Params. Where a library
// computes a CRC at run time and any engine will do, it names Kvasir::Crc::ImageEngine<P> (kvasir/Runtime/
// CrcImageEngine.hpp): the engine the image chose for P - a hardware one where the image names one that computes P,
// else Engine<P>. This header is the software alone: the standard library, nothing of the port.
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>

namespace Kvasir::Crc {

template<std::unsigned_integral T>
struct Params {
    std::uint8_t width;    // 1 .. the bits of T
    T            poly;     // normal (MSB-first) form, without the x^width term
    T            init;     // as the catalogue gives it (not reflected)
    bool         refin;    // bytes enter LSB first
    bool         refout;   // the register is reflected before xorout
    T            xorout;
    T            check;   // the CRC of the ASCII bytes "123456789"
};

namespace Detail {
    template<std::unsigned_integral T>
    constexpr T reflect(T        v,
                        unsigned width) {
        T r{};
        for(unsigned i = 0; i < width; ++i) {
            if(((v >> i) & 1U) != 0) { r = static_cast<T>(r | (T{1} << (width - 1 - i))); }
        }
        return r;
    }
}   // namespace Detail

/// The low `width` bits of T set: a `width`-bit CRC's register (a hardware engine's result masked to it too).
template<std::unsigned_integral T>
constexpr T widthMask(unsigned width) {
    return width == sizeof(T) * 8 ? static_cast<T>(~T{}) : static_cast<T>((T{1} << width) - 1);
}

// TableSize: 0 bitwise (no table), 4 (2 bits a step), 16 (a nibble a step), 256 (a byte a step).
template<auto P, std::size_t TableSize = 16>
    requires(TableSize == 0 || TableSize == 4 || TableSize == 16 || TableSize == 256)
class Engine {
public:
    using value_type                       = std::remove_cvref_t<decltype(P.poly)>;
    static constexpr auto        params    = P;
    static constexpr std::size_t tableSize = TableSize;

private:
    using T                        = value_type;
    static constexpr unsigned bits = sizeof(T) * 8;
    static constexpr unsigned w    = P.width;
    static_assert(w >= 1 && w <= bits,
                  "the CRC's width does not fit its value type");
    static_assert(bits >= 8,
                  "the value type is at least a byte");
    // Not reflected, the register is kept MSB-aligned in T: a width below the type's (CRC-7) needs
    // only a shift at the start and the end.
    static constexpr unsigned shift         = bits - w;
    static constexpr unsigned stepBits      = TableSize == 256 ? 8
                                            : TableSize == 16  ? 4
                                            : TableSize == 4   ? 2
                                                               : 1;
    static constexpr T        polyAligned   = static_cast<T>(P.poly << shift);
    static constexpr T        polyReflected = Detail::reflect<T>(P.poly, w);
    static constexpr T        top           = static_cast<T>(T{1} << (bits - 1));

    static constexpr T stepBy(T        r,
                              unsigned n) {
        for(unsigned i = 0; i < n; ++i) {
            if constexpr(P.refin) {
                r = (r & 1U) != 0 ? static_cast<T>((r >> 1) ^ polyReflected)
                                  : static_cast<T>(r >> 1);
            } else {
                r = (r & top) != 0 ? static_cast<T>((r << 1) ^ polyAligned)
                                   : static_cast<T>(r << 1);
            }
        }
        return r;
    }

    static constexpr auto makeTable() {
        std::array<T, TableSize> t{};
        for(std::size_t i = 0; i < TableSize; ++i) {
            T const seed = P.refin ? static_cast<T>(i) : static_cast<T>(T(i) << (bits - stepBits));
            t[i]         = stepBy(seed, stepBits);
        }
        return t;
    }

    static constexpr T initialRegister() {
        return P.refin ? Detail::reflect<T>(P.init, w) : static_cast<T>(P.init << shift);
    }

    T reg_ = initialRegister();

public:
    static constexpr std::array<T, TableSize> table = makeTable();

    constexpr Engine() = default;

    constexpr Engine& update(std::byte b) {
        auto const v = std::to_integer<std::uint8_t>(b);
        if constexpr(P.refin) {
            reg_ = static_cast<T>(reg_ ^ v);
            for(unsigned s = 0; s < 8; s += stepBits) {
                if constexpr(TableSize == 0) {
                    reg_ = stepBy(reg_, 1);
                } else {
                    T const rest
                      = bits > stepBits ? static_cast<T>(reg_ >> stepBits) : static_cast<T>(0);
                    reg_ = static_cast<T>(
                      rest ^ table[static_cast<std::size_t>(reg_ & (TableSize - 1))]);
                }
            }
        } else {
            reg_ = static_cast<T>(reg_ ^ static_cast<T>(T(v) << (bits - 8)));
            for(unsigned s = 0; s < 8; s += stepBits) {
                if constexpr(TableSize == 0) {
                    reg_ = stepBy(reg_, 1);
                } else {
                    T const rest
                      = bits > stepBits ? static_cast<T>(reg_ << stepBits) : static_cast<T>(0);
                    reg_ = static_cast<T>(
                      rest ^ table[static_cast<std::size_t>(reg_ >> (bits - stepBits))]);
                }
            }
        }
        return *this;
    }

    constexpr Engine& update(std::span<std::byte const> data) {
        for(auto const b : data) { update(b); }
        return *this;
    }

    constexpr Engine& update(std::span<std::uint8_t const> data) {
        for(auto const b : data) { update(std::byte{b}); }
        return *this;
    }

    constexpr Engine& update(std::string_view text) {
        for(char const c : text) { update(static_cast<std::byte>(c)); }
        return *this;
    }

    /// The CRC of everything so far. The state is unchanged: update() may go on, and a CRC can be
    /// read part way through a record.
    [[nodiscard]] constexpr T finish() const {
        static_assert(selfTest(), "the parameters do not give their own check value");
        return finishUnchecked();
    }

    /// Go on from a finished CRC, as zlib's crc32(previous, data): resume(compute(a)).update(b)
    /// is compute(a + b). Only where input and output are reflected alike.
    [[nodiscard]] static constexpr Engine resume(T finished)
        requires(P.refin == P.refout)
    {
        Engine  e;
        T const r = static_cast<T>(finished ^ P.xorout);
        e.reg_    = P.refin ? r : static_cast<T>(r << shift);
        return e;
    }

    [[nodiscard]] static constexpr T compute(std::span<std::byte const> data) {
        return Engine{}.update(data).finish();
    }

    [[nodiscard]] static constexpr T compute(std::span<std::uint8_t const> data) {
        return Engine{}.update(data).finish();
    }

    [[nodiscard]] static constexpr T compute(std::string_view text) {
        return Engine{}.update(text).finish();
    }

private:
    [[nodiscard]] constexpr T finishUnchecked() const {
        T r = P.refin ? reg_ : static_cast<T>(reg_ >> shift);
        if constexpr(P.refin != P.refout) { r = Detail::reflect<T>(r, w); }
        return static_cast<T>((r ^ P.xorout) & widthMask<T>(w));
    }

    // In a member function body the class is complete; evaluated where finish() is instantiated.
    static consteval bool selfTest() {
        Engine e;
        e.update(std::string_view{"123456789"});
        return e.finishUnchecked() == P.check;
    }
};

// Named parameter sets, by their reveng catalogue names; Engine asserts each check value at its first use.
namespace Presets {
    // clang-format off
    inline constexpr Params<std::uint8_t>  crc8Smbus   {8,  0x07,        0x00,        false, false, 0x00,        0xF4};
    inline constexpr Params<std::uint8_t>  crc8MaximDow{8,  0x31,        0x00,        true,  true,  0x00,        0xA1};        // 1-Wire
    inline constexpr Params<std::uint8_t>  crc8Nrsc5   {8,  0x31,        0xFF,        false, false, 0x00,        0xF7};        // Sensirion, AHT20
    inline constexpr Params<std::uint8_t>  crc8Htu21d  {8,  0x31,        0x00,        false, false, 0x00,        0xA2};        // no catalogue name
    inline constexpr Params<std::uint8_t>  crc7Mmc     {7,  0x09,        0x00,        false, false, 0x00,        0x75};        // SD commands
    inline constexpr Params<std::uint16_t> crc16Xmodem {16, 0x1021,      0x0000,      false, false, 0x0000,      0x31C3};      // SD data
    inline constexpr Params<std::uint16_t> crc16Ibm3740{16, 0x1021,      0xFFFF,      false, false, 0x0000,      0x29B1};      // "CCITT-FALSE"
    inline constexpr Params<std::uint16_t> crc16Kermit {16, 0x1021,      0x0000,      true,  true,  0x0000,      0x2189};      // "CCITT" (true)
    inline constexpr Params<std::uint16_t> crc16X25    {16, 0x1021,      0xFFFF,      true,  true,  0xFFFF,      0x906E};
    inline constexpr Params<std::uint32_t> crc32IsoHdlc{32, 0x04C11DB7U, 0xFFFFFFFFU, true,  true,  0xFFFFFFFFU, 0xCBF43926U}; // zlib, PNG, Ethernet
    inline constexpr Params<std::uint32_t> crc32Bzip2  {32, 0x04C11DB7U, 0xFFFFFFFFU, false, false, 0xFFFFFFFFU, 0xFC891918U};
    inline constexpr Params<std::uint32_t> crc32Mpeg2  {32, 0x04C11DB7U, 0xFFFFFFFFU, false, false, 0x00000000U, 0x0376E6E7U};
    inline constexpr Params<std::uint32_t> crc32c      {32, 0x1EDC6F41U, 0xFFFFFFFFU, true,  true,  0xFFFFFFFFU, 0xE3069283U};
    // clang-format on
}   // namespace Presets

template<std::size_t N = 0>
using Crc8Smbus = Engine<Presets::crc8Smbus, N>;
template<std::size_t N = 0>
using Crc8MaximDow = Engine<Presets::crc8MaximDow, N>;
template<std::size_t N = 0>
using Crc8Sensirion = Engine<Presets::crc8Nrsc5, N>;
template<std::size_t N = 0>
using Crc8Htu21d = Engine<Presets::crc8Htu21d, N>;
template<std::size_t N = 0>
using Crc7Mmc = Engine<Presets::crc7Mmc, N>;
template<std::size_t N = 16>
using Crc16Xmodem = Engine<Presets::crc16Xmodem, N>;
// the "CCITT" Kvasir means everywhere (the sniffer's crc16Ccitt, the aglio protocols)
template<std::size_t N = 16>
using Crc16Ccitt = Engine<Presets::crc16Ibm3740, N>;
template<std::size_t N = 16>
using Crc32T = Engine<Presets::crc32IsoHdlc, N>;
// CRC-32/ISO-HDLC with a nibble table: what Persistent<T> and the flash self-check use
using Crc32 = Crc32T<>;

/// The `type` + `calc(span)` shape aglio's packager takes: `using Crc = Kvasir::Crc::Calc<Kvasir::Crc::Crc16Ccitt<0>>;`.
template<typename E>
struct Calc {
    using type = typename E::value_type;

    static constexpr type calc(std::span<std::byte const> data) { return E::compute(data); }
};

}   // namespace Kvasir::Crc
