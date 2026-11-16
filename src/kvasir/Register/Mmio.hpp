#pragma once
// The register layer's one raw memory access: a load or a store of a T at an address, always inlined (RAM functions
// reach registers through it; gcc at -Og or with sanitizers would otherwise leave a plain inline call in flash).
// With KVASIR_REGISTER_MOCK every access goes to Kvasir::Test::readAt / writeAt (tests/kvasir_test.hpp), which
// records it with its width T. Registers go through the field API (apply) or RegisterTraits; a new access path
// calls Mmio instead of a cast of its own, so the mock sees it.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

#ifdef KVASIR_REGISTER_MOCK
// Test seam: a test provides the definitions (see tests/kvasir_test.hpp).
namespace Kvasir { namespace Test {
    template<typename TRegType>
    TRegType readAt(unsigned address);

    template<typename TRegType>
    void writeAt(unsigned address,
                 TRegType);
}}   // namespace Kvasir::Test
#endif

namespace Kvasir { namespace Register {
    template<typename T>
    struct Mmio {
        static_assert(std::is_unsigned_v<T>,
                      "Mmio: a register or memory word is an unsigned integer");

        [[gnu::always_inline]] static T load(std::uintptr_t address) {
#ifdef KVASIR_REGISTER_MOCK
            return ::Kvasir::Test::readAt<T>(static_cast<unsigned>(address));
#else
            return *reinterpret_cast<T volatile*>(address);
#endif
        }

        [[gnu::always_inline]] static void store(std::uintptr_t address,
                                                 T              v) {
#ifdef KVASIR_REGISTER_MOCK
            ::Kvasir::Test::writeAt<T>(static_cast<unsigned>(address), v);
#else
            *reinterpret_cast<T volatile*>(address) = v;
#endif
        }
    };

    // A run of T from a base address (a flash page, a vector table): the address becomes a pointer ONCE, where the run
    // is made, and element i is that pointer indexed (base[i]) - what a loop over a volatile pointer compiled to. An
    // address computed per element (`Mmio<T>::load(base + 4 * i)`) is integer arithmetic behind an int-to-pointer
    // conversion inside the loop: clang does not fold it into the addressing mode (+4..56 B in a page loop, other
    // registers), and gcc numbers its values otherwise (operands swapped). Make the run before the loop.
    template<typename T>
    class MmioWords {
        static_assert(std::is_unsigned_v<T>,
                      "MmioWords: a memory word is an unsigned integer");

    public:
        [[gnu::always_inline]] explicit MmioWords(std::uintptr_t base)
#ifdef KVASIR_REGISTER_MOCK
          : base_{base}
#else
          : base_{reinterpret_cast<T volatile*>(base)}
#endif
        {
        }

        // one load of element i (at base + i * sizeof(T))
        [[nodiscard,
          gnu::always_inline]] T
        load(std::size_t i) const {
#ifdef KVASIR_REGISTER_MOCK
            return ::Kvasir::Test::readAt<T>(static_cast<unsigned>(base_ + i * sizeof(T)));
#else
            return base_[i];
#endif
        }

        // one store of element i
        [[gnu::always_inline]] void store(std::size_t i,
                                          T           v) const {
#ifdef KVASIR_REGISTER_MOCK
            ::Kvasir::Test::writeAt<T>(static_cast<unsigned>(base_ + i * sizeof(T)), v);
#else
            base_[i] = v;
#endif
        }

    private:
#ifdef KVASIR_REGISTER_MOCK
        std::uintptr_t base_;
#else
        T volatile* base_;
#endif
    };

    // `count` bytes from an address on into out, as one std::memcpy from it (memory read as bytes: flash and NVM
    // records). memcpy is a library call: not for a RAM function (Memory::loadWords is). The mock reads byte by byte.
    [[gnu::always_inline]] inline void mmioLoadBytes(std::uintptr_t address,
                                                     std::byte*     out,
                                                     std::size_t    count) {
#ifdef KVASIR_REGISTER_MOCK
        for(std::size_t i = 0; i != count; ++i) {
            out[i] = static_cast<std::byte>(
              ::Kvasir::Test::readAt<std::uint8_t>(static_cast<unsigned>(address + i)));
        }
#else
        std::memcpy(out, reinterpret_cast<void const*>(address), count);
#endif
    }

#ifndef KVASIR_REGISTER_MOCK
    namespace Detail {
        // `n` bytes between device memory and a buffer, either way, for memory that takes aligned accesses of at most
        // a word and is handed over by a register access before or after: whole words where both ends are word
        // aligned, bytes otherwise, kept between what comes before and after.
        //
        // The memory clobber, here and at the end, keeps the copy in program order: the hardware reads the memory once
        // a register is written and writes it before a flag says so, both volatile accesses that plain stores and
        // loads may otherwise move across. On every core, the memcpy path too.
        static inline void copyAligned(void*       dst,
                                       void const* src,
                                       std::size_t n) {
            asm volatile("" ::: "memory");
    #ifdef __ARM_FEATURE_UNALIGNED
            // Keeps the compiler from making a memcpy of the loops, and unaligned word accesses of that.
            asm volatile("" : "+r"(dst), "+r"(src));

            auto*       dstByte = static_cast<std::uint8_t*>(dst);
            auto const* srcByte = static_cast<std::uint8_t const*>(src);

            constexpr std::size_t wordSize = sizeof(std::uint32_t);

            // bytes until dst is word aligned
            while(n > 0 && (reinterpret_cast<std::uintptr_t>(dstByte) % wordSize) != 0) {
                *dstByte++ = *srcByte++;
                --n;
            }

            // both ends word aligned now: words
            if((reinterpret_cast<std::uintptr_t>(srcByte) % wordSize) == 0) {
                // The words lie over whatever the caller handed in (a packet's bytes, a driver's buffer) and over
                // the device memory: a std::uint32_t has no access to those objects ([basic.lval]), so the word
                // type may alias. Through void*: the alignment is checked above, which -Wcast-align cannot see.
                using Word [[gnu::may_alias]] = std::uint32_t;
                auto*       dstWord           = static_cast<Word*>(static_cast<void*>(dstByte));
                auto const* srcWord = static_cast<Word const*>(static_cast<void const*>(srcByte));

                while(n >= wordSize) {
                    *dstWord++ = *srcWord++;
                    n -= wordSize;
                }

                dstByte = reinterpret_cast<std::uint8_t*>(dstWord);
                srcByte = reinterpret_cast<std::uint8_t const*>(srcWord);
            }

            // the rest: bytes
            while(n > 0) {
                *dstByte++ = *srcByte++;
                --n;
            }
    #else
            std::memcpy(dst, src, n);
    #endif
            asm volatile("" ::: "memory");
        }
    }   // namespace Detail
#endif

    // `count` bytes from an address on into out, for device memory that takes aligned accesses of at most a word (a
    // USB controller's buffer RAM): words where the address and `out` are word aligned, bytes otherwise, and nothing
    // of it moved across the register accesses around it (Detail::copyAligned). The mock reads byte by byte.
    [[gnu::always_inline]] inline void mmioLoadBytesAligned(std::uintptr_t address,
                                                            std::byte*     out,
                                                            std::size_t    count) {
#ifdef KVASIR_REGISTER_MOCK
        for(std::size_t i = 0; i != count; ++i) {
            out[i] = static_cast<std::byte>(
              ::Kvasir::Test::readAt<std::uint8_t>(static_cast<unsigned>(address + i)));
        }
#else
        Detail::copyAligned(out, reinterpret_cast<void const*>(address), count);
#endif
    }

    // the other way: `count` bytes from `in` to an address on. The mock writes byte by byte.
    [[gnu::always_inline]] inline void mmioStoreBytesAligned(std::uintptr_t   address,
                                                             std::byte const* in,
                                                             std::size_t      count) {
#ifdef KVASIR_REGISTER_MOCK
        for(std::size_t i = 0; i != count; ++i) {
            ::Kvasir::Test::writeAt<std::uint8_t>(static_cast<unsigned>(address + i),
                                                  static_cast<std::uint8_t>(in[i]));
        }
#else
        Detail::copyAligned(reinterpret_cast<void*>(address), in, count);
#endif
    }

    // `count` bytes from an address on set to `value`, as one std::memset (it writes whole words where it can): a
    // device memory's buffer RAM cleared before the device is used. The mock writes byte by byte.
    [[gnu::always_inline]] inline void mmioFillBytes(std::uintptr_t address,
                                                     std::byte      value,
                                                     std::size_t    count) {
#ifdef KVASIR_REGISTER_MOCK
        for(std::size_t i = 0; i != count; ++i) {
            ::Kvasir::Test::writeAt<std::uint8_t>(static_cast<unsigned>(address + i),
                                                  static_cast<std::uint8_t>(value));
        }
#else
        std::memset(reinterpret_cast<void*>(address), static_cast<int>(value), count);
#endif
    }

    // `count` bytes from `in` to an address on, as one std::memcpy to it (memory written as bytes: a buffer the
    // hardware takes over afterwards). memcpy is a library call: not for a RAM function. The mock writes byte by byte.
    [[gnu::always_inline]] inline void mmioStoreBytes(std::uintptr_t   address,
                                                      std::byte const* in,
                                                      std::size_t      count) {
#ifdef KVASIR_REGISTER_MOCK
        for(std::size_t i = 0; i != count; ++i) {
            ::Kvasir::Test::writeAt<std::uint8_t>(static_cast<unsigned>(address + i),
                                                  static_cast<std::uint8_t>(in[i]));
        }
#else
        std::memcpy(reinterpret_cast<void*>(address), in, count);
#endif
    }

    // One T from an address as a plain, aligned load (not volatile): memory that does not change while the program
    // runs, which the compiler may merge, move or leave out like any read of a constant. The mock reads it as one T.
    template<typename T>
    [[gnu::always_inline]] inline T mmioLoadPlain(std::uintptr_t address) {
        static_assert(std::is_unsigned_v<T>,
                      "a plain load is one unsigned word; a record is loadBytes + std::bit_cast");
#ifdef KVASIR_REGISTER_MOCK
        return ::Kvasir::Test::readAt<T>(static_cast<unsigned>(address));
#else
        T v;
        std::memcpy(&v, reinterpret_cast<T const*>(address), sizeof(T));
        return v;
#endif
    }
}}   // namespace Kvasir::Register
