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

    // One T from an address as a plain, aligned load (not volatile): memory that does not change while the program
    // runs, which the compiler may merge, move or leave out like any read of a constant. The mock reads it as one T.
    template<typename T>
    [[gnu::always_inline]] inline T mmioLoadPlain(std::uintptr_t address) {
#ifdef KVASIR_REGISTER_MOCK
        return ::Kvasir::Test::readAt<T>(static_cast<unsigned>(address));
#else
        T v;
        std::memcpy(&v, reinterpret_cast<T const*>(address), sizeof(T));
        return v;
#endif
    }
}}   // namespace Kvasir::Register
