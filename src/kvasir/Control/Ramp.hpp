#pragma once
// Moving a value towards a target in steps: a slew limiter (Ramp) and a proportional fade
// (approach). No clock inside: one step per call.

#include <algorithm>

namespace Kvasir::Control {

/// A slew limiter: step() moves the value towards the target by at most `up` (rising) or `down`
/// (falling) and lands on it exactly.
template<typename T>
class Ramp {
public:
    constexpr Ramp(T up,
                   T down,
                   T start = T{})
      : up_{up}
      , down_{down}
      , value_{start}
      , target_{start} {}

    constexpr void setTarget(T t) { target_ = t; }

    constexpr void reset(T v) {
        value_  = v;
        target_ = v;
    }

    constexpr T step() {
        if(value_ < target_) {
            value_ = target_ - value_ > up_ ? value_ + up_ : target_;
        } else if(target_ < value_) {
            value_ = value_ - target_ > down_ ? value_ - down_ : target_;
        }
        return value_;
    }

    [[nodiscard]] constexpr bool reached() const { return value_ == target_; }

    [[nodiscard]] constexpr T value() const { return value_; }

    [[nodiscard]] constexpr T target() const { return target_; }

private:
    T up_;
    T down_;
    T value_;
    T target_;
};

/// One fade step: `rate` of the remaining way, at least `minStep`, landing exactly on the target.
template<typename T>
[[nodiscard]] constexpr T approach(T current,
                                   T target,
                                   T rate,
                                   T minStep) {
    T const diff     = target - current;
    T const distance = diff < T{} ? -diff : diff;
    T const step     = std::min(std::max(distance * rate, minStep), distance);
    return diff < T{} ? current - step : current + step;
}

}   // namespace Kvasir::Control
