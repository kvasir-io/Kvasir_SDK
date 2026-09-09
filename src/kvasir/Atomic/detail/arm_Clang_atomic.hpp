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

// The fetch-and-op operations a core without exclusive accesses (the Cortex-M0+: no
// ldrex/strex) cannot do inline: clang calls these for every std::atomic fetch_add,
// fetch_sub, fetch_and, fetch_or and fetch_xor there (exchange, compare_exchange and the
// 8-byte fetch_add are in arm_Common_atomic.hpp). On the M33 they are inline instructions
// and these are never referenced. Each one runs under the shim's lock.
#define KVASIR_ATOMIC_FETCH_OP(NAME, N, T, OP)                                           \
    [[gnu::used]] inline T __atomic_fetch_##NAME##_##N(void volatile* ptr, T val, int) { \
        CommonAtomic::ShimLock guard;                                                    \
        auto* const            p   = reinterpret_cast<T volatile*>(ptr);                 \
        T const                old = *p;                                                 \
        *p                         = static_cast<T>(old OP val);                         \
        return old;                                                                      \
    }
#define KVASIR_ATOMIC_FETCH_OPS(N, T)    \
    KVASIR_ATOMIC_FETCH_OP(add, N, T, +) \
    KVASIR_ATOMIC_FETCH_OP(sub, N, T, -) \
    KVASIR_ATOMIC_FETCH_OP(and, N, T, &) \
    KVASIR_ATOMIC_FETCH_OP(or, N, T, |)  \
    KVASIR_ATOMIC_FETCH_OP(xor, N, T, ^)

KVASIR_ATOMIC_FETCH_OPS(1,
                        unsigned char)
KVASIR_ATOMIC_FETCH_OPS(2,
                        unsigned short)
KVASIR_ATOMIC_FETCH_OPS(4,
                        unsigned)
KVASIR_ATOMIC_FETCH_OP(sub,
                       8,
                       unsigned long long,
                       -)
KVASIR_ATOMIC_FETCH_OP(and,
                       8,
                       unsigned long long,
                         &)
KVASIR_ATOMIC_FETCH_OP(or,
                       8,
                       unsigned long long,
                       |)
KVASIR_ATOMIC_FETCH_OP(xor,
                       8,
                       unsigned long long,
                       ^)
#undef KVASIR_ATOMIC_FETCH_OPS
#undef KVASIR_ATOMIC_FETCH_OP
}
