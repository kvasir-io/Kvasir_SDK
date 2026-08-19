#pragma once
#include <array>
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
          return (*reinterpret_cast<std::remove_cvref_t<F> const*>(s))(args...);
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

    // Memberwise instead of placement-new over *this: a callback may reinstall
    // itself from inside its own invocation, and reusing the storage of the
    // object being invoked is not something to rely on.
    template<typename F>
        requires(!Detail::IsStaticFunctionV<F>)
    constexpr StaticFunction& operator=(F&& f) {
        StaticFunction const tmp{std::forward<F>(f)};
        storage    = tmp.storage;
        invoke_ptr = tmp.invoke_ptr;
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
        StaticFunction const tmp{other};   // see operator=(F&&) above
        storage    = tmp.storage;
        invoke_ptr = tmp.invoke_ptr;
        return *this;
    }

    constexpr operator bool() const { return invoke_ptr != nullptr; }

    constexpr void reset() { invoke_ptr = nullptr; }

    template<typename... AArgs>
    constexpr R operator()(AArgs&&... args) const {
        assert(invoke_ptr != nullptr);
        return std::invoke(invoke_ptr, storage.data(), std::forward<AArgs>(args)...);
    }

private:
    using Storage_t = std::array<std::byte, Size>;
    // a plain pointer rather than Storage_t const&, so the invoker stays valid when
    // converted to a larger StaticFunction
    using Invoke_ptr_t = R (*)(std::byte const*,
                               Args...);

    Storage_t    storage{};
    Invoke_ptr_t invoke_ptr{};
};

}   // namespace Kvasir
