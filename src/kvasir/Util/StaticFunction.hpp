#pragma once
#include <array>
#include <cassert>
#include <cstddef>
#include <functional>
#include <memory>

namespace Kvasir {

template<typename, std::size_t>
struct StaticFunction;

template<typename R, typename... Args, std::size_t Size>
struct StaticFunction<R(Args...), Size> {
    constexpr StaticFunction() = default;

    constexpr StaticFunction(StaticFunction const& other)            = default;
    constexpr StaticFunction& operator=(StaticFunction const& other) = default;

    constexpr StaticFunction(StaticFunction&& other)            = default;
    constexpr StaticFunction& operator=(StaticFunction&& other) = default;

    constexpr ~StaticFunction() = default;

    template<typename F>
    constexpr StaticFunction(F&& f)
      : invoke_ptr{[](Storage_t const& s,
                      Args... args) -> R {
          return (*reinterpret_cast<std::remove_cvref_t<F> const*>(s.data()))(args...);
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

    template<typename F>
    constexpr StaticFunction& operator=(F&& f) {
        new(this) StaticFunction{std::forward<F>(f)};
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
        new(this) StaticFunction{other};
        return *this;
    }

    constexpr operator bool() const { return invoke_ptr != nullptr; }

    constexpr void reset() { invoke_ptr = nullptr; }

    template<typename... AArgs>
    constexpr R operator()(AArgs&&... args) const {
        assert(invoke_ptr != nullptr);
        return std::invoke(invoke_ptr, storage, std::forward<AArgs>(args)...);
    }

private:
    using Storage_t    = std::array<std::byte, Size>;
    using Invoke_ptr_t = R (*)(Storage_t const&,
                               Args...);

    Storage_t    storage;
    Invoke_ptr_t invoke_ptr{};
};

}   // namespace Kvasir
