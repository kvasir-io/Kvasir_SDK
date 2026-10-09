#pragma once
#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace Kvasir {

template<typename, std::size_t>
struct StaticFunction;

namespace Detail {
    // keeps the greedy StaticFunction(F&&) constructor from hijacking copy/move construction
    template<typename T>
    struct IsStaticFunction : std::false_type {};

    template<typename Signature, std::size_t Size>
    struct IsStaticFunction<StaticFunction<Signature, Size>> : std::true_type {};

    template<typename T>
    constexpr bool IsStaticFunctionV = IsStaticFunction<std::remove_cvref_t<T>>::value;
}   // namespace Detail

template<typename R, typename... Args, std::size_t Size>
struct StaticFunction<R(Args...), Size> {
    // lets the converting constructor reach a differently sized StaticFunction's storage
    template<typename, std::size_t>
    friend struct StaticFunction;

    constexpr StaticFunction() = default;

    constexpr StaticFunction(StaticFunction const& other)            = default;
    constexpr StaticFunction& operator=(StaticFunction const& other) = default;

    constexpr StaticFunction(StaticFunction&& other)            = default;
    constexpr StaticFunction& operator=(StaticFunction&& other) = default;

    constexpr ~StaticFunction() = default;

    template<typename F>
        requires(!Detail::IsStaticFunctionV<F>)
    constexpr StaticFunction(F&& f)
      : invoke_ptr{[](std::byte const* s,
                      Args... args) -> R {
          // Calls a copy of the function object - its own size, not the slot's: a function that
          // replaces itself (or is replaced by a nested interrupt) while it runs keeps its own
          // captures to the end. Via void*: storage is aligned for F (static_assert below), gcc
          // -Wcast-align cannot tell.
          std::remove_cvref_t<F> const copy
            = *static_cast<std::remove_cvref_t<F> const*>(static_cast<void const*>(s));
          return copy(args...);
      }} {
        using FF = std::remove_cvref_t<F>;
        static_assert(std::is_trivially_destructible_v<FF>,
                      "only trivially destructible functions");
        static_assert(std::is_trivially_copyable_v<FF>, "only trivially copyable functions");
        static_assert(Size >= sizeof(FF), "function too big to store");
        static_assert(std::alignment_of_v<StaticFunction> >= std::alignment_of_v<FF>,
                      "function is overaligned");

        new(storage.data()) FF{std::forward<F>(f)};
    }

    // publish(), so a slot an interrupt calls can be assigned while that interrupt is live
    template<typename F>
        requires(!Detail::IsStaticFunctionV<F>)
    constexpr StaticFunction& operator=(F&& f) {
        publish(std::forward<F>(f));
        return *this;
    }

    template<std::size_t OtherSize>
    constexpr StaticFunction(StaticFunction<R(Args...),
                                            OtherSize> const& other)
      : invoke_ptr{other.invoke_ptr} {
        static_assert(Size >= OtherSize, "other function too big to store");
        std::memcpy(storage.data(), other.storage.data(), OtherSize);
    }

    template<std::size_t OtherSize>
    constexpr StaticFunction& operator=(StaticFunction<R(Args...),
                                                       OtherSize> const& other) {
        publish(other);
        return *this;
    }

    // Assign `f` (a callable, or a StaticFunction of this or a smaller size) so that an
    // interrupt calling this object meanwhile never calls half of the old function and half
    // of the new one: the invoker is cleared first, the captures written, the new invoker
    // stored last, each step a single-word store or kept in order by a compiler fence. An
    // interrupt that runs in between sees the old function, no function, or the new one --
    // and no function means a call that does not happen, so whoever arms the interrupt still
    // assigns first. One core: the interrupt sees this thread's stores in program order, the
    // compiler is all that has to be kept from reordering them.
    //
    // Copy and move assignment stay the defaulted, trivial ones (queues of requests depend on
    // the type being trivially copyable): use publish() for a slot an interrupt reads.
    template<typename F>
    constexpr void publish(F&& f) {
        StaticFunction const tmp{std::forward<F>(f)};
        if consteval {
            storage    = tmp.storage;
            invoke_ptr = tmp.invoke_ptr;
        } else {
            storeInvoker(nullptr);
            std::atomic_signal_fence(std::memory_order_seq_cst);
            storage = tmp.storage;
            std::atomic_signal_fence(std::memory_order_seq_cst);
            storeInvoker(tmp.invoke_ptr);
        }
    }

    constexpr operator bool() const { return loadInvoker() != nullptr; }

    constexpr void reset() {
        if consteval {
            invoke_ptr = nullptr;
        } else {
            storeInvoker(nullptr);
        }
    }

    // The invoker calls a copy of the captures (sizeof the function object), read after the
    // invoker: a function that replaces itself (or is replaced by a nested interrupt) while it
    // runs keeps its own captures to the end, and publish()'s order means an invoker read here
    // never belongs to other captures. Not for a call that a higher-priority context may
    // publish() into while it runs: that direction (an interrupt assigning what thread code
    // calls) no driver has.
    template<typename... AArgs>
    constexpr R operator()(AArgs&&... args) const {
        if consteval {
            return std::invoke(invoke_ptr, storage.data(), std::forward<AArgs>(args)...);
        } else {
            auto const fn = loadInvoker();
            assert(fn != nullptr);
            std::atomic_signal_fence(std::memory_order_seq_cst);
            return std::invoke(fn, storage.data(), std::forward<AArgs>(args)...);
        }
    }

private:
    using Storage_t = std::array<std::byte, Size>;
    // a plain pointer rather than Storage_t const&, so the invoker stays valid when
    // converted to a larger StaticFunction
    using Invoke_ptr_t = R (*)(std::byte const*,
                               Args...);

    // One word, stored and loaded whole: never torn, never split or merged by the compiler.
    // std::atomic_ref on the plain member keeps StaticFunction trivially copyable (a
    // std::atomic member would not be); on the Cortex-M0+ it is the inline ldr/str of
    // +atomics-32, as std::atomic is. libc++ has no atomic_ref<T const> yet (C++26, P3323):
    // the load goes through a non-const reference and only reads.
    static_assert(std::atomic_ref<Invoke_ptr_t>::required_alignment <= alignof(Invoke_ptr_t));

    constexpr Invoke_ptr_t loadInvoker() const {
        if consteval {
            return invoke_ptr;
        } else {
            return std::atomic_ref<Invoke_ptr_t>{const_cast<Invoke_ptr_t&>(invoke_ptr)}.load(
              std::memory_order_relaxed);
        }
    }

    void storeInvoker(Invoke_ptr_t p) {
        std::atomic_ref<Invoke_ptr_t>{invoke_ptr}.store(p, std::memory_order_relaxed);
    }

    Storage_t    storage{};
    Invoke_ptr_t invoke_ptr{};
};

}   // namespace Kvasir
