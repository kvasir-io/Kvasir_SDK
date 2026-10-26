#pragma once

// UART vocabulary shared by the chip families: the frame format of a driver config, and the
// two shapes a byte stream has, so code that only moves bytes (a GNSS parser, a console) takes
// any family's UART:
//
//     template<Kvasir::Hal::Uart::ByteSource U> void pump() {
//         std::optional<std::byte> b;
//         while(U::receive(b)) { if(b) { parse(*b); } else { resync(); } }
//     }
//
//   ByteSource: `static bool receive(std::optional<std::byte>& out)` - false: nothing there;
//               true with a value: one byte; true with nullopt: a mark in the stream (a framing
//               error or an overrun lost bytes before the next one) where the family reports one.
//   ByteSink:   `static std::size_t send(std::span<std::byte const>)` - how many bytes were taken
//               (a queue: what fit; a DMA driver: all of them, or 0 while the last send runs);
//               `static bool txIdle()` - everything taken has left the shifter.

#include <concepts>
#include <cstddef>
#include <optional>
#include <span>

namespace Kvasir { namespace Hal { namespace Uart {
    enum class DataBits {
        bits5,
        bits6,
        bits7,
        bits8,
        bits9
    };   // a family without 9-bit frames refuses _9

    enum class Parity { none, odd, even };

    enum class StopBits { stop1, stop2 };

    template<typename U>
    concept ByteSource = requires(std::optional<std::byte>& b) {
        { U::receive(b) } -> std::same_as<bool>;
    };

    template<typename U>
    concept ByteSink = requires(std::span<std::byte const> s) {
        { U::send(s) } -> std::same_as<std::size_t>;
        { U::txIdle() } -> std::same_as<bool>;
    };
}}}   // namespace Kvasir::Hal::Uart
