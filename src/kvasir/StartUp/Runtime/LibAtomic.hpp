#pragma once
// Part of StartUp.hpp (the TU with main), included only through StartUp/Runtime/All.hpp at StartUp.hpp's end: the
// libatomic stand-in. There is no libatomic in these images: where a core cannot do an atomic operation inline, clang
// and gcc call these entry points, defined here once per image, all under one lock (Shim::Lock). Strong definitions;
// [[gnu::used]] because only compiler-generated libcalls name them, which LTO sees only at codegen.
//
// Which entry points an image gets is what its (core, compiler) calls - a missing one is a link error naming it:
//   every Arm core       the 8-byte ones (no doubleword exclusive access: Armv7-M DDI0403E A3.4 "LDREXD and STREXD
//                        are not supported in Armv7-M"; Armv6-M has no exclusives at all, DDI0419E A3.4; Armv8-M
//                        Mainline: none found in DDI0553B.y) and the generic sized-at-run-time ones (an atomic of odd
//                        size or over 8 bytes)
//   gcc on Armv6-M       the 1-, 2- and 4-byte exchange, compare-exchange and fetch-ops, and __atomic_test_and_set
//   clang on Armv6-M     the same read-modify-writes under their __sync_* names: clang builds v6-M with +atomics-32
//                        (cmake/arm_clang.cmake), so its loads and stores of up to 32 bits are inline
// Nothing <= 4 bytes on a core with exclusives: gcc calls a 1/2/4-byte __atomic_* entry point it can see defined
// instead of inlining ldrex/strex, which in a multicore image mixes a lock-based RMW with an exclusive one on the same
// object (a lost update).
//
// Survey of what an image calls: llvm-objdump -d <image>.elf, the bl/b targets named __atomic_* / __sync_* (llvm-nm -u
// over the objects shows nothing: they are LTO bitcode).
#include "kvasir/Atomic/Atomic.hpp"
#include "kvasir/Common/Tags.hpp"
#include "kvasir/Port/Capabilities.hpp"
#include "kvasir/Port/Hooks.hpp"
#include "kvasir/StartUp/Runtime/LibAtomicBlocks.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>

#ifdef __arm__

namespace Kvasir::Atomic::Shim {

// The lock every entry point runs under. On one core, masking interrupts is all it takes. On a multicore build
// (KVASIR_MULTICORE, set by CMake when CORE1_STACK_SIZE is given) masking interrupts says nothing to the other core, so
// a cross-core lock is added underneath: interrupts off first, then the lock, so no ISR can spin on a lock its own
// thread holds.
    #if defined(KVASIR_MULTICORE) && KVASIR_MULTICORE && KVASIR_CORE_ARMV8M_MAIN
// A spinlock on the exclusive accesses, written in assembly rather than on std::atomic_flag to make it visible that
// nothing here can call back into the shim: the 1-byte exchange is inline ldaexb/strexb, never a library call.
struct Lock {
    static inline std::uint8_t word{};

    Kvasir::Core::IrqGuard irq{};

    Lock() {
        std::uint32_t seen{};
        std::uint32_t failed{};
        asm volatile(
          "1:\n"
          "ldaexb %0, [%2]\n"
          "cmp %0, #0\n"
          "bne 1b\n"
          "movs %1, #1\n"
          "strexb %0, %1, [%2]\n"
          "cmp %0, #0\n"
          "bne 1b\n"
          : "=&r"(seen), "=&r"(failed)
          : "r"(std::addressof(word))
          : "memory", "cc");
    }

    ~Lock() {
        std::uint32_t const zero{};
        asm volatile("stlb %0, [%1]" : : "r"(zero), "r"(std::addressof(word)) : "memory");
    }

    Lock(Lock const&)            = delete;
    Lock& operator=(Lock const&) = delete;

    // For a stopped other core, and nothing else: a core that was reset (or parked in its NMI, which the interrupt
    // mask does not hold off) while inside a shim call still holds the word, and the next shim call on the surviving
    // core would spin on it forever. Called through onOtherCoreStopped() from SecondaryCore::reset() and the chip's
    // Port::StopOtherCore, when the other core is provably stopped and this core cannot be inside a shim call (the
    // lock is only ever held with interrupts masked, never across a call to either).
    static void forceRelease() {
        std::uint32_t const zero{};
        asm volatile("stlb %0, [%1]" : : "r"(zero), "r"(std::addressof(word)) : "memory");
    }
};
    #elif defined(KVASIR_MULTICORE) && KVASIR_MULTICORE && KVASIR_CORE_ARMV6M
// No exclusives (the Cortex-M0+): the chip's hardware lock (the Port::CrossCoreLock hook) does what ldaexb/strexb do
// above, under the same interrupt mask. On the RP2040 that is an SIO spinlock, and every std::atomic read-modify-write
// comes through here, since the core cannot do one inline.
// a variable template, so an incomplete type is `false` here and not an error of its own
template<typename T>
constexpr bool isComplete = requires { sizeof(T); };

// A class template so the hook is read late (Port::LateT on Dep): the members' bodies are instantiated at the end of
// the TU, after the chip's specialisation, wherever this header stands in it.
template<typename Dep>
struct LockT {
    using CrossCoreLock = Kvasir::Port::LateT<Kvasir::Port::CrossCoreLock<Kvasir::Tag::User>, Dep>;

    Kvasir::Core::IrqGuard irq{};

    LockT() {
        static_assert(
          isComplete<CrossCoreLock>,
          "KVASIR_MULTICORE on a core without exclusives needs the chip's "
          "Port::CrossCoreLock<Tag::User> (chip/CrossCoreLock.hpp, included by the chip's "
          "chip/StartUp.hpp)");
        CrossCoreLock::acquire();
    }

    ~LockT() { CrossCoreLock::release(); }

    LockT(LockT const&)            = delete;
    LockT& operator=(LockT const&) = delete;

    static void forceRelease() { CrossCoreLock::release(); }
};

using Lock = LockT<void>;
    #elif defined(KVASIR_MULTICORE) && KVASIR_MULTICORE
        #error "KVASIR_MULTICORE on this core: the atomic shim has no cross-core lock for it"
    #else
struct Lock {
    Kvasir::Core::IrqGuard irq{};

    static void forceRelease() {}
};
    #endif

}   // namespace Kvasir::Atomic::Shim

// The sized entry points: N is the operand's size (Word<T> asserts it). Memory orders are ignored: the lock is a full
// barrier on every core that comes here.
    #define KVASIR_SHIM_LOAD_STORE(N, T)                                             \
        T               __atomic_load_##N(void const volatile*, int);                \
        [[gnu::used]] T __atomic_load_##N(void const volatile* ptr, int) {           \
            Kvasir::Atomic::Shim::Lock const guard;                                  \
            T                                v{};                                    \
            Kvasir::Atomic::Shim::load(Kvasir::Atomic::Shim::Word<T>{}, ptr, &v);    \
            return v;                                                                \
        }                                                                            \
        void               __atomic_store_##N(void volatile*, T, int);               \
        [[gnu::used]] void __atomic_store_##N(void volatile* ptr, T val, int) {      \
            Kvasir::Atomic::Shim::Lock const guard;                                  \
            Kvasir::Atomic::Shim::store(Kvasir::Atomic::Shim::Word<T>{}, ptr, &val); \
        }

    #define KVASIR_SHIM_XCHG_OPS(N, T)                                                              \
        T               __atomic_exchange_##N(void volatile*, T, int);                              \
        [[gnu::used]] T __atomic_exchange_##N(void volatile* ptr, T val, int) {                     \
            Kvasir::Atomic::Shim::Lock const guard;                                                 \
            T                                old{};                                                 \
            Kvasir::Atomic::Shim::exchange(Kvasir::Atomic::Shim::Word<T>{}, ptr, &val, &old);       \
            return old;                                                                             \
        }                                                                                           \
        bool               __atomic_compare_exchange_##N(void volatile*, void*, T, bool, int, int); \
        [[gnu::used]] bool __atomic_compare_exchange_##N(void volatile* ptr,                        \
                                                         void*          expected,                   \
                                                         T              desired,                    \
                                                         bool,                                      \
                                                         int,                                       \
                                                         int) {                                     \
            Kvasir::Atomic::Shim::Lock const guard;                                                 \
            return Kvasir::Atomic::Shim::compareExchange(Kvasir::Atomic::Shim::Word<T>{},           \
                                                         ptr,                                       \
                                                         expected,                                  \
                                                         &desired);                                 \
        }

    #define KVASIR_SHIM_FETCH_OP(NAME, N, T, OP)                                                \
        T               __atomic_fetch_##NAME##_##N(void volatile*, T, int);                    \
        [[gnu::used]] T __atomic_fetch_##NAME##_##N(void volatile* ptr, T val, int) {           \
            Kvasir::Atomic::Shim::Lock const guard;                                             \
            return Kvasir::Atomic::Shim::fetchOp<T>(ptr, val, [](T a, T b) { return a OP b; }); \
        }
    #define KVASIR_SHIM_FETCH_OPS(N, T)    \
        KVASIR_SHIM_FETCH_OP(add, N, T, +) \
        KVASIR_SHIM_FETCH_OP(sub, N, T, -) \
        KVASIR_SHIM_FETCH_OP(and, N, T, &) \
        KVASIR_SHIM_FETCH_OP(or, N, T, |)  \
        KVASIR_SHIM_FETCH_OP(xor, N, T, ^)

    // The read-modify-writes under the names clang calls on v6-M with +atomics-32. The __sync names are compiler
    // builtins and cannot be defined under their own name, hence the asm labels.
    #define KVASIR_SHIM_SYNC_FETCH_OP(NAME, N, T, EXPR)                                       \
        T kvasir_sync_fetch_and_##NAME##_##N(T volatile*,                                     \
                                             T) __asm__("__sync_fetch_and_" #NAME "_" #N);    \
        [[gnu::used]] T kvasir_sync_fetch_and_##NAME##_##N(T volatile* ptr, T val) {          \
            Kvasir::Atomic::Shim::Lock const guard;                                           \
            return Kvasir::Atomic::Shim::fetchOp<T>(ptr, val, [](T a, T b) { return EXPR; }); \
        }
    #define KVASIR_SHIM_SYNC_OPS(N, T)                                                        \
        KVASIR_SHIM_SYNC_FETCH_OP(add, N, T, a + b)                                           \
        KVASIR_SHIM_SYNC_FETCH_OP(sub, N, T, a - b)                                           \
        KVASIR_SHIM_SYNC_FETCH_OP(and, N, T, a& b)                                            \
        KVASIR_SHIM_SYNC_FETCH_OP(or, N, T, a | b)                                            \
        KVASIR_SHIM_SYNC_FETCH_OP(xor, N, T, a ^ b)                                           \
        KVASIR_SHIM_SYNC_FETCH_OP(nand, N, T, ~(a & b))                                       \
        T kvasir_sync_lock_test_and_set_##N(T volatile*,                                      \
                                            T) __asm__("__sync_lock_test_and_set_" #N);       \
        [[gnu::used]] T kvasir_sync_lock_test_and_set_##N(T volatile* ptr, T val) {           \
            Kvasir::Atomic::Shim::Lock const guard;                                           \
            T                                old{};                                           \
            Kvasir::Atomic::Shim::exchange(Kvasir::Atomic::Shim::Word<T>{}, ptr, &val, &old); \
            return old;                                                                       \
        }                                                                                     \
        /* on failure the block writes the value it found into `expected` */                  \
        T kvasir_sync_val_compare_and_swap_##N(T volatile*, T, T) __asm__(                    \
          "__sync_val_compare_and_swap_" #N);                                                 \
        [[gnu::used]] T kvasir_sync_val_compare_and_swap_##N(T volatile* ptr,                 \
                                                             T           expected,            \
                                                             T           desired) {           \
            Kvasir::Atomic::Shim::Lock const guard;                                           \
            static_cast<void>(                                                                \
              Kvasir::Atomic::Shim::compareExchange(Kvasir::Atomic::Shim::Word<T>{},          \
                                                    ptr,                                      \
                                                    &expected,                                \
                                                    &desired));                               \
            return expected;                                                                  \
        }                                                                                     \
        bool kvasir_sync_bool_compare_and_swap_##N(T volatile*, T, T) __asm__(                \
          "__sync_bool_compare_and_swap_" #N);                                                \
        [[gnu::used]] bool kvasir_sync_bool_compare_and_swap_##N(T volatile* ptr,             \
                                                                 T           expected,        \
                                                                 T           desired) {       \
            Kvasir::Atomic::Shim::Lock const guard;                                           \
            return Kvasir::Atomic::Shim::compareExchange(Kvasir::Atomic::Shim::Word<T>{},     \
                                                         ptr,                                 \
                                                         &expected,                           \
                                                         &desired);                           \
        }

extern "C" {
// every Arm core: 8 bytes
KVASIR_SHIM_LOAD_STORE(8,
                       unsigned long long)
KVASIR_SHIM_XCHG_OPS(8,
                     unsigned long long)
KVASIR_SHIM_FETCH_OPS(8,
                      unsigned long long)

// every Arm core: the generic entry points (size at run time, any alignment), on Bytes. The names are builtins on both
// compilers, hence the asm labels.
void kvasir_atomic_load(std::size_t,
                        void const volatile*,
                        void*,
                        int) __asm__("__atomic_load");

[[gnu::used]] void kvasir_atomic_load(std::size_t          size,
                                      void const volatile* src,
                                      void*                dest,
                                      int) {
    Kvasir::Atomic::Shim::Lock const guard;
    Kvasir::Atomic::Shim::load(Kvasir::Atomic::Shim::Bytes{size}, src, dest);
}

void kvasir_atomic_store(std::size_t,
                         void volatile*,
                         void const*,
                         int) __asm__("__atomic_store");

[[gnu::used]] void kvasir_atomic_store(std::size_t    size,
                                       void volatile* dest,
                                       void const*    src,
                                       int) {
    Kvasir::Atomic::Shim::Lock const guard;
    Kvasir::Atomic::Shim::store(Kvasir::Atomic::Shim::Bytes{size}, dest, src);
}

void kvasir_atomic_exchange(std::size_t,
                            void volatile*,
                            void const*,
                            void*,
                            int) __asm__("__atomic_exchange");

[[gnu::used]] void kvasir_atomic_exchange(std::size_t    size,
                                          void volatile* ptr,
                                          void const*    val,
                                          void*          ret,
                                          int) {
    Kvasir::Atomic::Shim::Lock const guard;
    Kvasir::Atomic::Shim::exchange(Kvasir::Atomic::Shim::Bytes{size}, ptr, val, ret);
}

bool kvasir_atomic_compare_exchange(std::size_t,
                                    void volatile*,
                                    void*,
                                    void const*,
                                    bool,
                                    int,
                                    int) __asm__("__atomic_compare_exchange");

[[gnu::used]] bool kvasir_atomic_compare_exchange(std::size_t    size,
                                                  void volatile* ptr,
                                                  void*          expected,
                                                  void const*    desired,
                                                  bool,
                                                  int,
                                                  int) {
    Kvasir::Atomic::Shim::Lock const guard;
    return Kvasir::Atomic::Shim::compareExchange(Kvasir::Atomic::Shim::Bytes{size},
                                                 ptr,
                                                 expected,
                                                 desired);
}

    #if KVASIR_CORE_ARMV6M && !defined(__clang__)
// gcc on a core without exclusives: every read-modify-write of up to 4 bytes is a call
KVASIR_SHIM_XCHG_OPS(1,
                     unsigned char)
KVASIR_SHIM_XCHG_OPS(2,
                     unsigned short)
KVASIR_SHIM_XCHG_OPS(4,
                     unsigned)
KVASIR_SHIM_FETCH_OPS(1,
                      unsigned char)
KVASIR_SHIM_FETCH_OPS(2,
                      unsigned short)
KVASIR_SHIM_FETCH_OPS(4,
                      unsigned)

// std::atomic_flag::test_and_set (its clear() is a plain store). gcc only: clang calls __sync_lock_test_and_set_1
// there, and refuses a redeclaration of this builtin.
bool __atomic_test_and_set(void volatile*,
                           int);

[[gnu::used]] bool __atomic_test_and_set(void volatile* ptr,
                                         int) {
    Kvasir::Atomic::Shim::Lock const guard;
    unsigned char const              set = 1;
    unsigned char                    old{};
    Kvasir::Atomic::Shim::exchange(Kvasir::Atomic::Shim::Word<unsigned char>{}, ptr, &set, &old);
    return old != 0;
}
    #endif

    #if KVASIR_CORE_ARMV6M && defined(__clang__)
KVASIR_SHIM_SYNC_OPS(1,
                     unsigned char)
KVASIR_SHIM_SYNC_OPS(2,
                     unsigned short)
KVASIR_SHIM_SYNC_OPS(4,
                     unsigned)
    #endif
}

    #undef KVASIR_SHIM_SYNC_OPS
    #undef KVASIR_SHIM_SYNC_FETCH_OP
    #undef KVASIR_SHIM_FETCH_OPS
    #undef KVASIR_SHIM_FETCH_OP
    #undef KVASIR_SHIM_XCHG_OPS
    #undef KVASIR_SHIM_LOAD_STORE

// Declared in Atomic.hpp: what a stopped other core (a SecondaryCore reset, the chip's Port::StopOtherCore) means for
// the shim - release the lock it may have been holding. A no-op on a single-core build.
void Kvasir::Atomic::onOtherCoreStopped() { Kvasir::Atomic::Shim::Lock::forceRelease(); }

#endif
