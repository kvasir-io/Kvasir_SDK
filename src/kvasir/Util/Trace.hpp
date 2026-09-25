#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

/// A self-describing ring of 32-bit records in RAM, for tracing where a log line would change the
/// timing. Read over the probe with `kvasir_bench.py trace`, or with forEach(). One writer only.
///
///     using UsbTrace = Kvasir::Trace::Ring<"usb", 64, "us", "ints", "sie_status", "buff_status">;
///     UsbTrace::record(now_us, ints, sie, buff);
namespace Kvasir::Trace {

template<std::size_t N>
struct Name {
    std::array<char, N> text{};

    // NOLINTNEXTLINE(google-explicit-constructor): so that a literal is a template argument
    constexpr Name(char const (&literal)[N]) {
        for(std::size_t i = 0; i != N; ++i) { text[i] = literal[i]; }
    }

    static constexpr std::size_t length = N - 1;
};

template<std::size_t N>
Name(char const (&)[N]) -> Name<N>;

/// `layout` points at "name:field,field,...\0".
struct Header {
    static constexpr std::uint32_t Magic = 0x4352544BU;   // "KTRC" in memory

    std::uint32_t magic;
    std::uint16_t fields;
    std::uint16_t capacity;
    std::uint32_t count;   // records ever written; the next one goes to count % capacity
    char const*   layout;
};

template<Name RingName, std::size_t Capacity, Name... Fields>
struct Ring {
    static_assert(Capacity != 0 && (Capacity & (Capacity - 1)) == 0,
                  "the capacity of a trace ring is a power of two: record() masks instead of "
                  "dividing");
    static_assert(Capacity <= 0xFFFF,
                  "the capacity has to fit the header's 16 bits");
    static_assert(sizeof...(Fields) != 0,
                  "a trace ring needs at least one field");
    static_assert(sizeof(char const*) == 4 || sizeof(char const*) == 8);

    static constexpr std::size_t FieldCount = sizeof...(Fields);
    using Record                            = std::array<std::uint32_t, FieldCount>;

private:
    static constexpr std::size_t LayoutSize
      = RingName.length + 1 + (Fields.length + ...) + FieldCount;

    static constexpr std::array<char,
                                LayoutSize>
    makeLayout() {
        std::array<char, LayoutSize> out{};
        std::size_t                  at  = 0;
        auto const                   put = [&](auto const& name, char after) {
            for(std::size_t i = 0; i != name.length; ++i) { out[at++] = name.text[i]; }
            out[at++] = after;
        };
        put(RingName, ':');
        std::size_t left = FieldCount;
        ((put(Fields, --left == 0 ? '\0' : ',')), ...);
        return out;
    }

public:
    static constexpr std::array<char, LayoutSize> layout = makeLayout();

    struct Storage {
        Header                       header;
        std::array<Record, Capacity> records;
    };

    [[gnu::used]] static inline Storage storage{
      .header  = {Header::Magic,
                  static_cast<std::uint16_t>(FieldCount),
                  static_cast<std::uint16_t>(Capacity),
                  0, layout.data()},
      .records = {}
    };

    template<typename... Ts>
    [[gnu::always_inline]] static void record(Ts... values) {
        static_assert(sizeof...(Ts) == FieldCount,
                      "record() takes one value per field of the ring");
        auto const count                        = storage.header.count;
        storage.records[count & (Capacity - 1)] = Record{static_cast<std::uint32_t>(values)...};
        storage.header.count                    = count + 1;
    }

    static std::uint32_t count() { return storage.header.count; }

    static void clear() { storage.header.count = 0; }

    /// f(record), oldest first. Not while record() may run.
    template<typename F>
    static void forEach(F&& f) {
        auto const count = storage.header.count;
        auto const kept  = count < Capacity ? count : static_cast<std::uint32_t>(Capacity);
        for(std::uint32_t i = count - kept; i != count; ++i) {
            f(storage.records[i & (Capacity - 1)]);
        }
    }
};
}   // namespace Kvasir::Trace
