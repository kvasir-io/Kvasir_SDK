#pragma once
// A value kept in .noInit across resets, trusted only when its CRC matches. Place the object yourself:
//
//     struct CounterTag {
//         static constexpr std::uint32_t id      = Kvasir::persistentId("KBTG");
//         static constexpr std::uint16_t version = 1;
//     };
//     [[gnu::section(".noInit"), gnu::used]] inline Kvasir::Persistent<Counter, CounterTag> counter;
//
//     counter.update([](Counter& c) { ++c.failures; });   // from Counter{} when nothing valid is there
//     if(auto const c = counter.load()) { ... }            // nullopt: never stored, cleared, another build, corrupt
//
// Layout (what host tools decode, little-endian words):
//     word 0     Tag::id        (a four-character code, unique per kind of record)
//     word 1     Tag::version (15:0) | sizeof(T) (31:16)
//     words 2..  T, sizeof(T) / 4 words
//     last word  CRC-32/ISO-HDLC (zlib's crc32) over words 0 .. the end of T
// A header from another build (another id, version or size) fails like garbage does.
//
// Safe from a fault or panic handler: word loads and stores and the CRC loop, no library calls, no locks.
// Not safe against two writers at once (two cores, an ISR and main): the writer makes sure there is one.
#include "kvasir/Util/Crc.hpp"

#include <array>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>

namespace Kvasir {
namespace PersistentDetail {
    template<typename Tag>
    concept RecordTag = requires {
        { Tag::id } -> std::convertible_to<std::uint32_t>;
        { Tag::version } -> std::convertible_to<std::uint16_t>;
    };
}   // namespace PersistentDetail

/// "KFLT" -> the little-endian word a hex dump shows as that text: a Tag::id.
consteval std::uint32_t persistentId(char const (&s)[5]) {
    std::uint32_t v = 0;
    for(int i = 3; i >= 0; --i) {
        v = static_cast<std::uint32_t>(v << 8U)
          | static_cast<std::uint8_t>(s[static_cast<std::size_t>(i)]);
    }
    return v;
}

template<typename T, PersistentDetail::RecordTag Tag>
class Persistent {
    static_assert(std::is_trivially_copyable_v<T> && std::is_trivially_default_constructible_v<T>,
                  "a Persistent value is read back as raw words after a reset");
    static_assert(sizeof(T) % 4 == 0 && alignof(T) <= 4,
                  "a Persistent value is whole words");
    static_assert(sizeof(T) <= 0xFFFF,
                  "the size has 16 bits in the header");

public:
    static constexpr std::size_t   valueWords = sizeof(T) / 4;
    static constexpr std::size_t   words      = 2 + valueWords + 1;
    static constexpr std::uint32_t header1
      = std::uint32_t{Tag::version} | (static_cast<std::uint32_t>(sizeof(T)) << 16U);

private:
    // volatile: read after a reset, which LTO cannot see (it would narrow or drop the stores). No
    // initialiser: the object stays in .noInit's NOLOAD section, untouched by the startup code.
    std::uint32_t volatile w_[words];

    using Raw = std::array<std::uint32_t, words>;

    Raw raw() const {
        Raw r;
        for(std::size_t i = 0; i != words; ++i) { r[i] = w_[i]; }
        return r;
    }

    static std::uint32_t crcOf(Raw const& r) {
        Crc::Crc32 e;
        for(std::size_t i = 0; i != words - 1; ++i) {
            auto const w = r[i];
            for(unsigned b = 0; b != 4; ++b) { e.update(static_cast<std::byte>(w >> (8U * b))); }
        }
        return e.finish();
    }

public:
    using value_type = T;
    using tag_type   = Tag;

    Persistent()                             = default;   // trivial: nothing stored at static init
    Persistent(Persistent const&)            = delete;
    Persistent& operator=(Persistent const&) = delete;

    /// The value if header and CRC match; nullopt for "never stored", "cleared", "from another
    /// build" and "corrupt" alike.
    [[nodiscard]] std::optional<T> load() const {
        auto const r = raw();
        if(r[0] != Tag::id || r[1] != header1 || r[words - 1] != crcOf(r)) { return std::nullopt; }
        std::array<std::uint32_t, valueWords> v;
        for(std::size_t i = 0; i != valueWords; ++i) { v[i] = r[2 + i]; }
        return std::bit_cast<T>(v);
    }

    /// Whether load() would give a value, without copying it out.
    [[nodiscard]] bool valid() const {
        auto const r = raw();
        return r[0] == Tag::id && r[1] == header1 && r[words - 1] == crcOf(r);
    }

    /// The id word cleared first, then the header, the value and the CRC, the id last: a reset
    /// anywhere in between leaves a record that fails load(), never a mix of old and new. Word by word
    /// with the CRC computed on the way, so a large T (a stack snapshot) needs no copy on the stack.
    void store(T const& value) {
        storeWords([&value](std::size_t i) {
            std::uint32_t w;
            std::memcpy(&w, reinterpret_cast<std::byte const*>(&value) + i * 4, 4);
            return w;
        });
    }

    /// store() for a value given word by word: `word(i)` is word i of T, called once each, in order.
    template<std::invocable<std::size_t> W>
    void storeWords(W&& word) {
        w_[0] = 0;
        Crc::Crc32 e;
        auto const feed = [&e](std::uint32_t v) {
            for(unsigned b = 0; b != 4; ++b) { e.update(static_cast<std::byte>(v >> (8U * b))); }
        };
        feed(Tag::id);
        feed(header1);
        w_[1] = header1;
        for(std::size_t i = 0; i != valueWords; ++i) {
            std::uint32_t const v = word(i);
            feed(v);
            w_[2 + i] = v;
        }
        w_[words - 1] = e.finish();
        w_[0]         = Tag::id;
    }

    /// f(T&) on the stored value, or on T{} when there is none; then store.
    template<std::invocable<T&> F>
    void update(F&& f) {
        T v = load().value_or(T{});
        std::forward<F>(f)(v);
        store(v);
    }

    /// Any CRC mismatch will do; the id too, so a dump does not show a stale header.
    void clear() {
        w_[words - 1] = ~w_[words - 1];
        w_[0]         = 0;
    }

    /// load() then clear(): read once, as Fault::takeLastFault() / Panic::takeLastPanic().
    [[nodiscard]] std::optional<T> take() {
        auto v = load();
        clear();
        return v;
    }
};
}   // namespace Kvasir
