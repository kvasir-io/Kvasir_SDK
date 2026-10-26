#pragma once

#include <algorithm>

namespace Kvasir {
template<typename ValueType, ValueType Min, ValueType Max>
struct SaturatingValue {
private:
    ValueType v{};

    static constexpr ValueType clamped(ValueType vv) { return std::clamp(vv, Min, Max); }

public:
    constexpr auto operator<=>(SaturatingValue const&) const = default;

    constexpr SaturatingValue() : SaturatingValue{ValueType{}} {}

    constexpr explicit SaturatingValue(ValueType vv) : v{clamped(vv)} {}

    constexpr operator ValueType() const { return v; }

    [[nodiscard]] constexpr ValueType value() const { return v; }

    constexpr SaturatingValue& operator++() {
        if(v != Max) { ++v; }
        return *this;
    }

    constexpr SaturatingValue& operator--() {
        if(v != Min) { --v; }
        return *this;
    }

    template<typename U>
    constexpr SaturatingValue& operator+=(U vv) {
        if(__builtin_add_overflow(v, vv, &v)) {
            if(vv > 0) {
                v = Max;
            } else {
                v = Min;
            }
        } else {
            v = clamped(v);
        }
        return *this;
    }

    template<typename U>
    constexpr SaturatingValue& operator-=(U vv) {
        if(__builtin_sub_overflow(v, vv, &v)) {
            if(vv < 0) {
                v = Max;
            } else {
                v = Min;
            }
        } else {
            v = clamped(v);
        }
        return *this;
    }
};
}   // namespace Kvasir
