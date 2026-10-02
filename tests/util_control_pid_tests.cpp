// Kvasir::Control::Pid: P only first, limits, anti-windup, no setpoint kick, dt <= 0, reset,
// and a closed loop against a first-order plant. Mostly at compile time.
#include "kvasir/Util/Control/Pid.hpp"
#include "test_harness.hpp"

#include <bit>
#include <chrono>
#include <cstdint>

namespace {
using Kvasir::Control::Pid;
using Kvasir::Control::PidGains;
using namespace std::chrono_literals;

constexpr PidGains<float> Gains{.kp     = 2.0f,
                                .ki     = 1.0f,
                                .kd     = 0.5f,
                                .tau    = 0.1f,
                                .outMin = 0.0f,
                                .outMax = 10.0f};

static_assert(
  [] {
      Pid<float> p{Gains};
      auto const u = p.update(5.0f, 4.0f, 1.0f);   // P only, dt ignored
      return u == 2.0f && p.integral() == 0.0f && p.derivative() == 0.0f;
  }(),
  "the first update is P only");

static_assert(
  [] {
      Pid<float> p{Gains};
      for(int i = 0; i < 100; ++i) {
          auto const u = p.update(100.0f, 0.0f, 0.1f);
          if(u < 0.0f || u > 10.0f) { return false; }
      }
      return true;
  }(),
  "the output never leaves the limits");

static_assert(
  [] {
      Pid<float> p{Gains};
      for(int i = 0; i < 100; ++i) { (void)p.update(100.0f, 0.0f, 0.1f); }
      // P = 200 is far past outMax: no room left for the integral
      return p.integral() <= 0.0f;
  }(),
  "the integral gets only the room P leaves (anti-windup)");

static_assert(
  [] {
      Pid<float> p{Gains};
      (void)p.update(1.0f, 1.0f, 0.1f);
      (void)p.update(1.0f, 1.0f, 0.1f);
      (void)p.update(9.0f, 1.0f, 0.1f);   // setpoint step, measurement constant
      return p.derivative() == 0.0f;
  }(),
  "a setpoint step does not kick the derivative");

static_assert(
  [] {
      Pid<float> p{Gains};
      (void)p.update(5.0f, 1.0f, 0.1f);
      (void)p.update(5.0f, 2.0f, 0.1f);
      auto const i = p.integral();
      auto const d = p.derivative();
      (void)p.update(5.0f, 3.0f, 0.0f);
      (void)p.update(5.0f, 4.0f, -1.0f);
      return p.integral() == i && p.derivative() == d;
  }(),
  "dt <= 0 holds I and D");

static_assert(
  [] {
      Pid<float> p{Gains};
      (void)p.update(5.0f, 1.0f, 0.1f);
      (void)p.update(5.0f, 2.0f, 0.1f);
      p.reset();
      auto const u = p.update(5.0f, 4.0f, 0.1f);
      return u == 2.0f && p.integral() == 0.0f;
  }(),
  "reset makes the next update P only again");

static_assert(
  [] {
      Pid<float> p{Gains};
      (void)p.update(5.0f, 1.0f, 0.1f);
      auto const a = p.update(5.0f, 2.0f, 0.1f);
      Pid<float> q{Gains};
      (void)q.update(5.0f, 1.0f, 100ms);
      auto const b = q.update(5.0f, 2.0f, 100ms);
      return a == b;
  }(),
  "a duration is the same dt in seconds");

// a first-order plant: x += (k*u - x) * dt / T
struct Plant {
    float x{};

    constexpr void step(float u,
                        float dt) {
        x += (4.0f * u - x) * dt / 2.0f;
    }
};

static_assert(
  [] {
      Pid<float> p{
        PidGains<float>{.kp     = 1.0f,
                        .ki     = 0.8f,
                        .kd     = 0.0f,
                        .tau    = 0.05f,
                        .outMin = 0.0f,
                        .outMax = 10.0f}
      };
      Plant plant{};
      for(int i = 0; i < 4000; ++i) { plant.step(p.update(20.0f, plant.x, 0.01f), 0.01f); }
      return plant.x > 19.8f && plant.x < 20.2f;
  }(),
  "PI settles within 1 % on a first-order plant");

static_assert(
  [] {
      Pid<float> p{
        PidGains<float>{.kp     = 1.0f,
                        .ki     = 0.0f,
                        .kd     = 0.0f,
                        .tau    = 0.05f,
                        .outMin = 0.0f,
                        .outMax = 10.0f}
      };
      Plant plant{};
      for(int i = 0; i < 4000; ++i) { plant.step(p.update(20.0f, plant.x, 0.01f), 0.01f); }
      // P only with gain 4: x = 4 * (20 - x) -> 16
      return plant.x > 15.9f && plant.x < 16.1f;
  }(),
  "P only keeps its offset (ki = 0)");

}   // namespace

int main() {
    Kvasir::Test::test("pid at run time");
    Pid<float> p{Gains};
    float volatile sp = 5.0f;
    CHECK(p.update(sp, 4.0f, 0.1f) == 2.0f);
    // +0 on a zero error with a positive kp: P + 0 + 0 is P
    Pid<float> z{Gains};
    CHECK(std::bit_cast<std::uint32_t>(z.update(sp, sp, 0.1f)) == 0U);
    return Kvasir::Test::report();
}
