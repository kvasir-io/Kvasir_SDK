#pragma once
// The operations behind the atomic shim's entry points (LibAtomic.hpp), run by the caller under the shim's lock. Only
// templates and inline functions, no entry point: host code includes this to test them (tests/libatomic_blocks_tests).
//
// One body per operation for both kinds of entry point; how it touches the atomic object is the Access:
//   Word<T>  one access of T - the sized entry points (__atomic_exchange_4, __sync_*_N, ...), whose object the
//            compiler aligns to its size. A plain inline load or store of the same object elsewhere (a flag's clear(),
//            a std::atomic store()) is one access too, so neither tears the other.
//   Bytes    memcpy/memcmp - the generic entry points (__atomic_load, ...), any size and alignment.
#include <cstddef>
#include <cstring>

namespace Kvasir::Atomic::Shim {

template<typename T>
struct Word {
    static_assert(sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8,
                  "a sized entry point's N is its type's size");

    static void read(void const volatile* obj,
                     void*                to) {
        *static_cast<T*>(to) = *static_cast<T const volatile*>(obj);
    }

    static void write(void volatile* obj,
                      void const*    from) {
        *static_cast<T volatile*>(obj) = *static_cast<T const*>(from);
    }

    // the object against a plain value
    [[nodiscard]] static bool holds(void const volatile* obj,
                                    void const*          value) {
        return *static_cast<T const volatile*>(obj) == *static_cast<T const*>(value);
    }

    // two plain values
    [[nodiscard]] static bool same(void const* a,
                                   void const* b) {
        return *static_cast<T const*>(a) == *static_cast<T const*>(b);
    }
};

// The generic entry points hand in plain memory, guarded by the shim's lock, as `volatile void*`.
// NOLINTBEGIN(cppcoreguidelines-pro-type-const-cast) // memcpy/memcmp of that memory
struct Bytes {
    std::size_t size;

    void read(void const volatile* obj,
              void*                to) const {
        std::memcpy(to, const_cast<void const*>(obj), size);
    }

    void write(void volatile* obj,
               void const*    from) const {
        std::memcpy(const_cast<void*>(obj), from, size);
    }

    [[nodiscard]] bool holds(void const volatile* obj,
                             void const*          value) const {
        return std::memcmp(const_cast<void const*>(obj), value, size) == 0;
    }

    [[nodiscard]] bool same(void const* a,
                            void const* b) const {
        return std::memcmp(a, b, size) == 0;
    }
};

// NOLINTEND(cppcoreguidelines-pro-type-const-cast)

template<typename Access>
[[gnu::always_inline]] inline void load(Access               access,
                                        void const volatile* obj,
                                        void*                to) {
    access.read(obj, to);
}

template<typename Access>
[[gnu::always_inline]] inline void store(Access         access,
                                         void volatile* obj,
                                         void const*    from) {
    access.write(obj, from);
}

// A plain atomic store (a flag's clear(), a std::atomic store()) is an inline str on the cores that need the shim and
// does not take its lock: a write here can land after another core's store and undo it. So an exchange that changes
// nothing writes nothing - a Spinlock waiter's test_and_set on a held flag (read 1, write 1) must not put back the 1
// its owner's unlock has just cleared, or the flag stays held by nobody (the RP2040 Spinlock and atomic_flag rely on
// it; tests/libatomic_blocks_tests pins it). A store that races any other read-modify-write here can still be lost.
template<typename Access>
[[gnu::always_inline]] inline void exchange(Access         access,
                                            void volatile* obj,
                                            void const*    value,
                                            void*          old) {
    access.read(obj, old);
    if(!access.same(old, value)) { access.write(obj, value); }
}

// true and `desired` stored when the object holds `expected`; else false and `expected` = what it holds
template<typename Access>
[[gnu::always_inline]] inline bool compareExchange(Access         access,
                                                   void volatile* obj,
                                                   void*          expected,
                                                   void const*    desired) {
    if(access.holds(obj, expected)) {
        access.write(obj, desired);
        return true;
    }
    access.read(obj, expected);
    return false;
}

// the old value; the object = op(old, value). Sized entry points only (there is no generic fetch-op).
template<typename T,
         typename Op>
[[gnu::always_inline]] inline T fetchOp(void volatile* obj,
                                        T              value,
                                        Op             op) {
    auto* const p   = static_cast<T volatile*>(obj);
    T const     old = *p;
    *p              = static_cast<T>(op(old, value));
    return old;
}

}   // namespace Kvasir::Atomic::Shim
