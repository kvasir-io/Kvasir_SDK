#pragma once
#include "core/Nvic.hpp"

#if defined(KVASIR_MULTICORE) && KVASIR_MULTICORE && !defined(__ARM_ARCH_8M_MAIN__)
    // A core without exclusive accesses cannot build the shim's cross-core lock itself: the
    // chip provides Kvasir::Atomic::CrossCoreLock (acquire / release) on a hardware primitive.
    #if __has_include("chip/CrossCoreLock.hpp")
        #include "chip/CrossCoreLock.hpp"
    #else
        #error                                                                                    \
          "KVASIR_MULTICORE on a core without exclusives needs the chip's chip/CrossCoreLock.hpp"
    #endif
#endif

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <type_traits>

static_assert(sizeof(unsigned char) == 1,
              "WTF");
static_assert(sizeof(unsigned short) == 2,
              "WTF");
static_assert(sizeof(unsigned) == 4,
              "WTF");
static_assert(sizeof(unsigned long long) == 8,
              "WTF");

namespace Kvasir { namespace Nvic {
    [[nodiscard]] inline bool primask() {
        unsigned result{};
        asm("mrs %0, primask" : "=r"(result)::"memory");
        return result != 0;
    }

    void inline disable_all() { asm("cpsid i" : : : "memory"); }

    [[nodiscard]] inline bool disable_all_and_get_old_state() {
        bool const p = primask();
        disable_all();
        return !p;
    }

    void inline enable_all() { asm("cpsie i" : : : "memory"); }

    struct Global {};

    template<typename T>
    struct InterruptGuard {
    private:
        bool oldState;

    public:
        constexpr InterruptGuard() {
            if(!std::is_constant_evaluated()) {
                if constexpr(std::is_same_v<Global, T>) {
                    oldState = disable_all_and_get_old_state();
                } else {
                    oldState = static_cast<bool>(get<0>(apply(Nvic::makeRead(T{}))));
                    apply(Nvic::makeDisable(T{}));
                }
            } else {
                oldState = false;
            }
        }

        InterruptGuard(InterruptGuard const&) = delete;

        constexpr InterruptGuard(InterruptGuard&& other) : oldState(other.oldState) {
            other.oldState = false;
        }

        InterruptGuard& operator=(InterruptGuard const&) = delete;

        constexpr InterruptGuard& operator=(InterruptGuard&& other) {
            if(this != std::addressof(other)) {
                oldState       = other.oldState;
                other.oldState = false;
            }
            return *this;
        }

        constexpr ~InterruptGuard() {
            if(oldState) {
                if constexpr(std::is_same_v<Global, T>) {
                    enable_all();
                } else {
                    apply(Nvic::makeEnable(T{}));
                }
                asm(
                  "nop\n"
                  "nop\n"
                  :
                  :
                  : "memory");
            }
        }
    };

    template<typename T>
    struct InterruptGuardAlwaysUnlock {
    public:
        InterruptGuardAlwaysUnlock() {
            if constexpr(std::is_same_v<Global, T>) {
                disable_all();
            } else {
                apply(Nvic::makeDisable(T{}));
            }
        }

        InterruptGuardAlwaysUnlock(InterruptGuardAlwaysUnlock const&)            = delete;
        InterruptGuardAlwaysUnlock(InterruptGuardAlwaysUnlock&&)                 = delete;
        InterruptGuardAlwaysUnlock& operator=(InterruptGuardAlwaysUnlock const&) = delete;
        InterruptGuardAlwaysUnlock& operator=(InterruptGuardAlwaysUnlock&&)      = delete;

        ~InterruptGuardAlwaysUnlock() {
            if constexpr(std::is_same_v<Global, T>) {
                enable_all();
            } else {
                apply(Nvic::makeEnable(T{}));
            }
            asm(
              "nop\n"
              "nop\n"
              :
              :
              : "memory");
        }
    };
}}   // namespace Kvasir::Nvic

namespace CommonAtomic {

// The lock the out-of-line atomics run under. On one core, masking interrupts is all it
// takes and is exactly what it always was. On a multicore build (KVASIR_MULTICORE, set by
// CMake when CORE1_STACK_SIZE is given) masking interrupts says nothing to the other core,
// so a spinlock on the exclusive accesses is added underneath: interrupts off first, then
// the lock, so no ISR can spin on a lock its own thread holds. Written in assembly rather
// than on std::atomic_flag to make it visible that nothing here can call back into the
// shim: the 1-byte exchange is inline ldaexb/strexb, never a library call. A core without
// exclusives (the Cortex-M0+) takes the chip's hardware lock instead, see below.
#if defined(KVASIR_MULTICORE) && KVASIR_MULTICORE && defined(__ARM_ARCH_8M_MAIN__)
struct ShimLock {
    static inline std::uint8_t word{};

    Kvasir::Nvic::InterruptGuard<Kvasir::Nvic::Global> irq{};

    ShimLock() {
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

    ~ShimLock() {
        std::uint32_t const zero{};
        asm volatile("stlb %0, [%1]" : : "r"(zero), "r"(std::addressof(word)) : "memory");
    }

    ShimLock(ShimLock const&)            = delete;
    ShimLock& operator=(ShimLock const&) = delete;

    // For a secondary core reset, and nothing else: a core that was reset while inside a
    // shim call still holds the word, and the next 8-byte atomic or CAS on the surviving
    // core would spin on it forever. Called from SecondaryCore::reset(), when the other
    // core is provably stopped and this core cannot be inside a shim call (the lock is
    // only ever held with interrupts masked, never across a call to reset()).
    static void forceRelease() {
        std::uint32_t const zero{};
        asm volatile("stlb %0, [%1]" : : "r"(zero), "r"(std::addressof(word)) : "memory");
    }
};
#elif defined(KVASIR_MULTICORE) && KVASIR_MULTICORE
// No exclusives (the Cortex-M0+): the chip's hardware lock does what ldaexb/strexb do
// above, under the same interrupt mask. On the RP2040 that is an SIO spinlock, and every
// std::atomic read-modify-write comes through here, since the core cannot do one inline.
struct ShimLock {
    Kvasir::Nvic::InterruptGuard<Kvasir::Nvic::Global> irq{};

    ShimLock() { Kvasir::Atomic::CrossCoreLock::acquire(); }

    ~ShimLock() { Kvasir::Atomic::CrossCoreLock::release(); }

    ShimLock(ShimLock const&)            = delete;
    ShimLock& operator=(ShimLock const&) = delete;

    static void forceRelease() { Kvasir::Atomic::CrossCoreLock::release(); }
};
#else
struct ShimLock : Kvasir::Nvic::InterruptGuard<Kvasir::Nvic::Global> {
    static void forceRelease() {}
};
#endif

template<typename T>
T atomic_load_block(void const volatile* ptr,
                    [[maybe_unused]] int memorder) {
    ShimLock guard;
    T        v = *reinterpret_cast<T const volatile*>(ptr);
    return v;
}

template<typename T>
void atomic_store_block(void volatile*       ptr,
                        T                    val,
                        [[maybe_unused]] int memorder) {
    ShimLock guard;
    *reinterpret_cast<T volatile*>(ptr) = val;
}

template<typename T>
T atomic_exchange_block(void volatile*       ptr,
                        T                    val,
                        [[maybe_unused]] int memorder) {
    ShimLock guard;
    T        old                        = *reinterpret_cast<T volatile*>(ptr);
    *reinterpret_cast<T volatile*>(ptr) = val;
    return old;
}

template<typename T>
bool atomic_compare_exchange_block(void volatile*        ptr,
                                   void*                 expected,
                                   T                     desired,
                                   [[maybe_unused]] bool weak,
                                   [[maybe_unused]] int  success_memorder,
                                   [[maybe_unused]] int  failure_memorder) {
    ShimLock guard;
    bool     ret{};
    if(*reinterpret_cast<T volatile*>(ptr) == *reinterpret_cast<T*>(expected)) {
        *reinterpret_cast<T volatile*>(ptr) = desired;
        ret                                 = true;
    } else {
        *reinterpret_cast<T*>(expected) = *reinterpret_cast<T volatile*>(ptr);
        ret                             = false;
    }
    return ret;
}

inline void atomic_load_mem_block(std::size_t          size,
                                  void const volatile* src,
                                  void*                dest,
                                  [[maybe_unused]] int memorder) {
    ShimLock guard;
    std::memcpy(dest, const_cast<void const*>(src), size);
}

inline void atomic_store_mem_block(std::size_t          size,
                                   void volatile*       dest,
                                   void const*          src,
                                   [[maybe_unused]] int memorder) {
    ShimLock guard;
    std::memcpy(const_cast<void*>(dest), src, size);
}

inline void atomic_exchange_mem_block(std::size_t          size,
                                      void volatile*       ptr,
                                      void const*          val,
                                      void*                ret,
                                      [[maybe_unused]] int memorder) {
    ShimLock guard;
    std::memcpy(ret, const_cast<void const*>(ptr), size);
    std::memcpy(const_cast<void*>(ptr), val, size);
}

inline bool atomic_compare_exchange_mem_block(std::size_t           size,
                                              void volatile*        ptr,
                                              void*                 expected,
                                              void const*           desired,
                                              [[maybe_unused]] bool weak,
                                              [[maybe_unused]] int  success_memorder,
                                              [[maybe_unused]] int  failure_memorder) {
    ShimLock guard;
    bool     ret{};
    if(std::memcmp(const_cast<void const*>(ptr), expected, size) == 0) {
        std::memcpy(const_cast<void*>(ptr), desired, size);
        ret = true;
    } else {
        std::memcpy(expected, const_cast<void const*>(ptr), size);
        ret = false;
    }
    return ret;
}

}   // namespace CommonAtomic

extern "C" {
[[gnu::used]] inline unsigned long long __atomic_load_8(void const volatile* ptr,
                                                        int                  memorder) {
    return CommonAtomic::atomic_load_block<unsigned long long>(ptr, memorder);
}

[[gnu::used]] inline void __atomic_store_8(void volatile*     ptr,
                                           unsigned long long val,
                                           int                memorder) {
    CommonAtomic::atomic_store_block<unsigned long long>(ptr, val, memorder);
}

[[gnu::used]] inline unsigned char __atomic_exchange_1(void volatile* ptr,
                                                       unsigned char  val,
                                                       int            memorder) {
    return CommonAtomic::atomic_exchange_block<unsigned char>(ptr, val, memorder);
}

[[gnu::used]] inline unsigned short __atomic_exchange_2(void volatile* ptr,
                                                        unsigned short val,
                                                        int            memorder) {
    return CommonAtomic::atomic_exchange_block<unsigned short>(ptr, val, memorder);
}

[[gnu::used]] inline unsigned __atomic_exchange_4(void volatile* ptr,
                                                  unsigned       val,
                                                  int            memorder) {
    return CommonAtomic::atomic_exchange_block<unsigned>(ptr, val, memorder);
}

[[gnu::used]] inline unsigned long long __atomic_exchange_8(void volatile*     ptr,
                                                            unsigned long long val,
                                                            int                memorder) {
    return CommonAtomic::atomic_exchange_block<unsigned long long>(ptr, val, memorder);
}

[[gnu::used]] inline bool __atomic_compare_exchange_1(void volatile* ptr,
                                                      void*          expected,
                                                      unsigned char  desired,
                                                      bool           weak,
                                                      int            success_memorder,
                                                      int            failure_memorder) {
    return CommonAtomic::atomic_compare_exchange_block<unsigned char>(ptr,
                                                                      expected,
                                                                      desired,
                                                                      weak,
                                                                      success_memorder,
                                                                      failure_memorder);
}

[[gnu::used]] inline bool __atomic_compare_exchange_2(void volatile* ptr,
                                                      void*          expected,
                                                      unsigned short desired,
                                                      bool           weak,
                                                      int            success_memorder,
                                                      int            failure_memorder) {
    return CommonAtomic::atomic_compare_exchange_block<unsigned short>(ptr,
                                                                       expected,
                                                                       desired,
                                                                       weak,
                                                                       success_memorder,
                                                                       failure_memorder);
}

[[gnu::used]] inline bool __atomic_compare_exchange_4(void volatile* ptr,
                                                      void*          expected,
                                                      unsigned       desired,
                                                      bool           weak,
                                                      int            success_memorder,
                                                      int            failure_memorder) {
    return CommonAtomic::atomic_compare_exchange_block<unsigned>(ptr,
                                                                 expected,
                                                                 desired,
                                                                 weak,
                                                                 success_memorder,
                                                                 failure_memorder);
}

[[gnu::used]] inline bool __atomic_compare_exchange_8(void volatile*     ptr,
                                                      void*              expected,
                                                      unsigned long long desired,
                                                      bool               weak,
                                                      int                success_memorder,
                                                      int                failure_memorder) {
    return CommonAtomic::atomic_compare_exchange_block<unsigned long long>(ptr,
                                                                           expected,
                                                                           desired,
                                                                           weak,
                                                                           success_memorder,
                                                                           failure_memorder);
}

[[gnu::used]] inline unsigned long long __atomic_fetch_add_8(void volatile*       ptr,
                                                             unsigned long long   val,
                                                             [[maybe_unused]] int memorder) {
    CommonAtomic::ShimLock   guard;
    auto&                    ref = *reinterpret_cast<unsigned long long volatile*>(ptr);
    unsigned long long const old = ref;
    ref                          = old + val;
    return old;
}
}

namespace Kvasir::Atomic {
// What a SecondaryCore reset has to do for the atomic shim: release the lock the dead core
// may have been holding. A no-op on a single-core build.
inline void onSecondaryCoreReset() { CommonAtomic::ShimLock::forceRelease(); }
}   // namespace Kvasir::Atomic
