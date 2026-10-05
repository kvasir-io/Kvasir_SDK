#pragma once
#include "arm_Common_atomic.hpp"

extern "C" {
// std::atomic_flag::test_and_set: gcc calls this on a core without exclusive accesses; its clear()
// is a plain store. gcc only: clang lowers test_and_set to __atomic_exchange_1 there and refuses a
// redeclaration of the builtin.
[[gnu::used]] inline bool __atomic_test_and_set(void volatile*       ptr,
                                                [[maybe_unused]] int memorder) {
    CommonAtomic::ShimLock guard;
    auto* const            p   = reinterpret_cast<unsigned char volatile*>(ptr);
    bool const             old = *p != 0;
    if(!old) { *p = 1; }   // as atomic_exchange_block: no write that could undo a clear()
    return old;
}
}
