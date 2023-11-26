#pragma once
#include "core/Nvic.hpp"

#include <cstddef>
#include <cstring>
#include <memory>
#include <type_traits>

static_assert(sizeof(unsigned char) == 1, "WTF");
static_assert(sizeof(unsigned short) == 2, "WTF");
static_assert(sizeof(unsigned) == 4, "WTF");
static_assert(sizeof(unsigned long long) == 8, "WTF");

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
        InterruptGuard&           operator=(InterruptGuard const&) = delete;
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
template<typename T>
T atomic_load_block(void const volatile* ptr, [[maybe_unused]] int memorder) {
    Kvasir::Nvic::InterruptGuard<Kvasir::Nvic::Global> guard;
    T v = *reinterpret_cast<T const volatile*>(ptr);
    return v;
}

template<typename T>
void atomic_store_block(void volatile* ptr, T val, [[maybe_unused]] int memorder) {
    Kvasir::Nvic::InterruptGuard<Kvasir::Nvic::Global> guard;
    *reinterpret_cast<T volatile*>(ptr) = val;
}

template<typename T>
T atomic_exchange_block(void volatile* ptr, T val, [[maybe_unused]] int memorder) {
    Kvasir::Nvic::InterruptGuard<Kvasir::Nvic::Global> guard;
    T                                                  old = *reinterpret_cast<T volatile*>(ptr);
    *reinterpret_cast<T volatile*>(ptr)                    = val;
    return old;
}

template<typename T>
bool atomic_compare_exchange_block(
  void volatile*        ptr,
  void*                 expected,
  T                     desired,
  [[maybe_unused]] bool weak,
  [[maybe_unused]] int  success_memorder,
  [[maybe_unused]] int  failure_memorder) {
    Kvasir::Nvic::InterruptGuard<Kvasir::Nvic::Global> guard;
    bool                                               ret{};
    if(*reinterpret_cast<T volatile*>(ptr) == *reinterpret_cast<T*>(expected)) {
        *reinterpret_cast<T volatile*>(ptr) = desired;
        ret                                 = true;
    } else {
        *reinterpret_cast<T*>(expected) = *reinterpret_cast<T volatile*>(ptr);
        ret                             = false;
    }
    return ret;
}

inline void atomic_load_mem_block(
  std::size_t          size,
  void const volatile* src,
  void*                dest,
  [[maybe_unused]] int memorder) {
    Kvasir::Nvic::InterruptGuard<Kvasir::Nvic::Global> guard;
    std::memcpy(dest, const_cast<void const*>(src), size);
}

inline void atomic_store_mem_block(
  std::size_t          size,
  void volatile*       dest,
  void const*          src,
  [[maybe_unused]] int memorder) {
    Kvasir::Nvic::InterruptGuard<Kvasir::Nvic::Global> guard;
    std::memcpy(const_cast<void*>(dest), src, size);
}

inline void atomic_exchange_mem_block(
  std::size_t          size,
  void volatile*       ptr,
  void const*          val,
  void*                ret,
  [[maybe_unused]] int memorder) {
    Kvasir::Nvic::InterruptGuard<Kvasir::Nvic::Global> guard;
    std::memcpy(ret, const_cast<void const*>(ptr), size);
    std::memcpy(const_cast<void*>(ptr), val, size);
}

inline bool atomic_compare_exchange_mem_block(
  std::size_t           size,
  void volatile*        ptr,
  void*                 expected,
  void const*           desired,
  [[maybe_unused]] bool weak,
  [[maybe_unused]] int  success_memorder,
  [[maybe_unused]] int  failure_memorder) {
    Kvasir::Nvic::InterruptGuard<Kvasir::Nvic::Global> guard;
    bool                                               ret{};
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
[[gnu::used]] inline unsigned long long __atomic_load_8(void const volatile* ptr, int memorder) {
    return CommonAtomic::atomic_load_block<unsigned long long>(ptr, memorder);
}
[[gnu::used]] inline void
__atomic_store_8(void volatile* ptr, unsigned long long val, int memorder) {
    CommonAtomic::atomic_store_block<unsigned long long>(ptr, val, memorder);
}

[[gnu::used]] inline unsigned char
__atomic_exchange_1(void volatile* ptr, unsigned char val, int memorder) {
    return CommonAtomic::atomic_exchange_block<unsigned char>(ptr, val, memorder);
}
[[gnu::used]] inline unsigned short
__atomic_exchange_2(void volatile* ptr, unsigned short val, int memorder) {
    return CommonAtomic::atomic_exchange_block<unsigned short>(ptr, val, memorder);
}
[[gnu::used]] inline unsigned __atomic_exchange_4(void volatile* ptr, unsigned val, int memorder) {
    return CommonAtomic::atomic_exchange_block<unsigned>(ptr, val, memorder);
}
[[gnu::used]] inline unsigned long long
__atomic_exchange_8(void volatile* ptr, unsigned long long val, int memorder) {
    return CommonAtomic::atomic_exchange_block<unsigned long long>(ptr, val, memorder);
}

[[gnu::used]] inline bool __atomic_compare_exchange_1(
  void volatile* ptr,
  void*          expected,
  unsigned char  desired,
  bool           weak,
  int            success_memorder,
  int            failure_memorder) {
    return CommonAtomic::atomic_compare_exchange_block<unsigned char>(
      ptr,
      expected,
      desired,
      weak,
      success_memorder,
      failure_memorder);
}
[[gnu::used]] inline bool __atomic_compare_exchange_2(
  void volatile* ptr,
  void*          expected,
  unsigned short desired,
  bool           weak,
  int            success_memorder,
  int            failure_memorder) {
    return CommonAtomic::atomic_compare_exchange_block<unsigned short>(
      ptr,
      expected,
      desired,
      weak,
      success_memorder,
      failure_memorder);
}
[[gnu::used]] inline bool __atomic_compare_exchange_4(
  void volatile* ptr,
  void*          expected,
  unsigned       desired,
  bool           weak,
  int            success_memorder,
  int            failure_memorder) {
    return CommonAtomic::atomic_compare_exchange_block<unsigned>(
      ptr,
      expected,
      desired,
      weak,
      success_memorder,
      failure_memorder);
}
[[gnu::used]] inline bool __atomic_compare_exchange_8(
  void volatile*     ptr,
  void*              expected,
  unsigned long long desired,
  bool               weak,
  int                success_memorder,
  int                failure_memorder) {
    return CommonAtomic::atomic_compare_exchange_block<unsigned long long>(
      ptr,
      expected,
      desired,
      weak,
      success_memorder,
      failure_memorder);
}
}
