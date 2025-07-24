#pragma once
#include "arm_Common_atomic.hpp"

#include <arm_acle.h>

namespace ClangAtomic {
template<typename T>
T atomic_load_nonblock(void const volatile* ptr,
                       int                  memorder) {
    switch(memorder) {
    case __ATOMIC_SEQ_CST:
        {
            __dmb(11);
        }
        break;
    case __ATOMIC_RELAXED:
    case __ATOMIC_ACQUIRE:
    case __ATOMIC_CONSUME:
        {
        }
        break;
    default:
        {
            __builtin_unreachable();
        }
    }

    T v = *reinterpret_cast<T const volatile*>(ptr);

    switch(memorder) {
    case __ATOMIC_ACQUIRE:
    case __ATOMIC_CONSUME:
    case __ATOMIC_SEQ_CST:
        {
            __dmb(11);
        }
        break;
    case __ATOMIC_RELAXED:
        {
        }
        break;
    default:
        {
            __builtin_unreachable();
        }
    }
    return v;
}

template<typename T>
void atomic_store_nonblock(void volatile* ptr,
                           T              val,
                           int            memorder) {
    switch(memorder) {
    case __ATOMIC_RELEASE:
    case __ATOMIC_SEQ_CST:
        {
            __dmb(11);
        }
        break;
    case __ATOMIC_RELAXED:
        {
        }
        break;
    default:
        {
            __builtin_unreachable();
        }
    }

    *reinterpret_cast<T volatile*>(ptr) = val;

    switch(memorder) {
    case __ATOMIC_SEQ_CST:
        {
            __dmb(11);
        }
        break;
    case __ATOMIC_RELAXED:
    case __ATOMIC_RELEASE:
        {
        }
        break;
    default:
        {
            __builtin_unreachable();
        }
    }
}
}   // namespace ClangAtomic

extern "C" {
#pragma redefine_extname __atomic_load_c __atomic_load
#pragma redefine_extname __atomic_store_c __atomic_store
#pragma redefine_extname __atomic_exchange_c __atomic_exchange
#pragma redefine_extname __atomic_compare_exchange_c __atomic_compare_exchange

inline void __atomic_load_c(size_t               size,
                            void const volatile* src,
                            void*                dest,
                            int                  memorder) {
    CommonAtomic::atomic_load_mem_block(size, src, dest, memorder);
}

inline void __atomic_store_c(size_t         size,
                             void volatile* dest,
                             void const*    src,
                             int            memorder) {
    CommonAtomic::atomic_store_mem_block(size, dest, src, memorder);
}

inline void __atomic_exchange_c(size_t         size,
                                void volatile* ptr,
                                void const*    val,
                                void*          ret,
                                int            memorder) {
    CommonAtomic::atomic_exchange_mem_block(size, ptr, val, ret, memorder);
}

inline bool __atomic_compare_exchange_c(size_t         size,
                                        void volatile* ptr,
                                        void*          expected,
                                        void const*    desired,
                                        bool           weak,
                                        int            success_memorder,
                                        int            failure_memorder) {
    return CommonAtomic::atomic_compare_exchange_mem_block(size,
                                                           ptr,
                                                           expected,
                                                           desired,
                                                           weak,
                                                           success_memorder,
                                                           failure_memorder);
}

[[gnu::used]] inline unsigned char __atomic_load_1(void const volatile* ptr,
                                                   int                  memorder) {
    return ClangAtomic::atomic_load_nonblock<unsigned char>(ptr, memorder);
}

[[gnu::used]] inline unsigned short __atomic_load_2(void const volatile* ptr,
                                                    int                  memorder) {
    return ClangAtomic::atomic_load_nonblock<unsigned short>(ptr, memorder);
}

[[gnu::used]] inline unsigned __atomic_load_4(void const volatile* ptr,
                                              int                  memorder) {
    return ClangAtomic::atomic_load_nonblock<unsigned>(ptr, memorder);
}

[[gnu::used]] inline void __atomic_store_1(void volatile* ptr,
                                           unsigned char  val,
                                           int            memorder) {
    ClangAtomic::atomic_store_nonblock<unsigned char>(ptr, val, memorder);
}

[[gnu::used]] inline void __atomic_store_2(void volatile* ptr,
                                           unsigned short val,
                                           int            memorder) {
    ClangAtomic::atomic_store_nonblock<unsigned short>(ptr, val, memorder);
}

[[gnu::used]] inline void __atomic_store_4(void volatile* ptr,
                                           unsigned       val,
                                           int            memorder) {
    ClangAtomic::atomic_store_nonblock<unsigned>(ptr, val, memorder);
}
}
