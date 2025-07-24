#pragma once

#include <algorithm>

namespace Kvasir {
template<typename ValueType, ValueType min, ValueType max>
struct SaturatingValue {
private:
    ValueType v{};

    static constexpr ValueType clamped(ValueType vv) { return std::clamp(vv, min, max); }

public:
    constexpr auto operator<=>(SaturatingValue const&) const = default;

    constexpr SaturatingValue() : SaturatingValue{ValueType{}} {}

    constexpr explicit SaturatingValue(ValueType vv) : v{clamped(vv)} {}

    constexpr operator ValueType() const { return v; }

    constexpr ValueType value() const { return v; }

    constexpr SaturatingValue& operator++() {
        if(v != max) {
            ++v;
        }
        return *this;
    }

    constexpr SaturatingValue& operator--() {
        if(v != min) {
            --v;
        }
        return *this;
    }

    template<typename U>
    constexpr SaturatingValue& operator+=(U vv) {
        if(__builtin_add_overflow(v, vv, &v)) {
            if(vv > 0) {
                v = max;
            } else {
                v = min;
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
                v = max;
            } else {
                v = min;
            }
        } else {
            v = clamped(v);
        }
        return *this;
    }
};
}   // namespace Kvasir
