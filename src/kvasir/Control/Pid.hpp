#pragma once
// A PID controller with output limits, anti-windup and a filtered derivative, in the band-limited
// form (trapezoid integral, derivative on the measurement through a first-order filter, Tustin).
//
//     inline constexpr Kvasir::Control::PidGains<float> Gains{
//       .kp = 0.3f, .ki = 0.0f, .kd = 0.2f, .tau = 0.7f, .outMin = 0.0f, .outMax = 10.0f};
//     Kvasir::Control::Pid<float> pid{Gains};
//     float const u = pid.update(setpoint, measured, now - lastAt);   // dt in seconds or a duration
//
// No clock inside: dt comes from the caller, so the controller runs in an ISR or a test alike. The
// first update after construction or reset() is P only and ignores dt. A positive kd damps (the
// derivative is taken on the measurement, so a setpoint step does not kick the output).
//
// Floating point only: every current user is. On a Cortex-M0+ (no FPU) every operation is a
// library call; the RP2350's M33 has a single-precision FPU - use float, never double.

#include <algorithm>
#include <chrono>
#include <concepts>

namespace Kvasir::Control {

template<std::floating_point T>
struct PidGains {
    T kp{};
    T ki{};
    T kd{};
    T tau{};   // derivative filter time constant in seconds
    T outMin{};
    T outMax{};
};

template<typename T>
class Pid {
    static_assert(std::floating_point<T>,
                  "Kvasir::Control::Pid is floating point only");

public:
    constexpr explicit Pid(PidGains<T> g) : g_{g} {}

    /// dt in seconds; dt <= 0 holds I and D (a zero tau would divide by zero).
    constexpr T update(T setpoint,
                       T measurement,
                       T dt) {
        T const error = setpoint - measurement;
        p_            = g_.kp * error;
        T out         = p_;
        if(primed_ && dt > T{}) {
            i_ += T{0.5} * g_.ki * dt * (error + prevError_);   // trapezoid
            // anti-windup: the integral gets only the room P leaves inside the limits
            T const iMax = g_.outMax > p_ ? g_.outMax - p_ : T{};
            T const iMin = g_.outMin < p_ ? g_.outMin - p_ : T{};
            i_           = std::clamp(i_, iMin, iMax);
            d_ = (-(T{2} * g_.kd * (measurement - prevMeasurement_)) + (T{2} * g_.tau - dt) * d_)
               / (T{2} * g_.tau + dt);
            out += i_ + d_;
        }
        primed_          = true;
        prevMeasurement_ = measurement;
        prevError_       = error;
        return out_      = std::clamp(out, g_.outMin, g_.outMax);
    }

    /// dt as a duration: converted through nanoseconds.
    template<typename Rep,
             typename Period>
    constexpr T update(T                             setpoint,
                       T                             measurement,
                       std::chrono::duration<Rep,
                                             Period> dt) {
        return update(
          setpoint,
          measurement,
          static_cast<T>(std::chrono::duration_cast<std::chrono::nanoseconds>(dt).count())
            / T{1000000000.0});
    }

    /// The next update is P only again (tip changed, heater off).
    constexpr void reset() {
        primed_ = false;
        i_      = T{};
        d_      = T{};
    }

    constexpr void setGains(PidGains<T> g) {
        g_ = g;
        i_ = std::clamp(i_, g.outMin, g.outMax);
    }

    [[nodiscard]] constexpr PidGains<T> const& gains() const { return g_; }

    [[nodiscard]] constexpr T proportional() const { return p_; }

    [[nodiscard]] constexpr T integral() const { return i_; }

    [[nodiscard]] constexpr T derivative() const { return d_; }

    [[nodiscard]] constexpr T output() const { return out_; }

private:
    PidGains<T> g_;
    T           p_{};
    T           i_{};
    T           d_{};
    T           out_{};
    T           prevError_{};
    T           prevMeasurement_{};
    bool        primed_{};
};

}   // namespace Kvasir::Control
