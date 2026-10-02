// Kvasir::Control averages, median, ramp and tables.
#include "kvasir/Util/Control/Average.hpp"
#include "kvasir/Util/Control/Interpolate.hpp"
#include "kvasir/Util/Control/Median.hpp"
#include "kvasir/Util/Control/Ramp.hpp"
#include "test_harness.hpp"

#include <array>
#include <cstdint>

namespace {
namespace Ctl = Kvasir::Control;

// ---- MovingAverage (scale's asserts, moved with the class) --------------------------------------
static_assert(
  [] {
      Ctl::MovingAverage<int, 4> m{};
      m.push(8);
      return m.average<float>() == 8.0f && !m.full();
  }(),
  "a window that is not yet full averages what arrived, not the empty slots");
static_assert(
  [] {
      Ctl::MovingAverage<std::int32_t, 4> m{};
      for(auto const v : {1, 2, 3, 4, 5}) { m.push(v); }
      return m.full() && m.average<float>() == 3.5f;
  }(),
  "the oldest sample drops out of a full window");
static_assert(!Ctl::MovingAverage<float,
                                  4>{}
                 .average()
                 .has_value(),
              "no mean before a sample");
static_assert(
  [] {
      Ctl::MovingAverage<float, 8> m{};
      m.push(100.0f);
      m.fill(2.0f);
      m.push(10.0f);
      return m.average() == 3.0f;
  }(),
  "fill() stands in for N samples");
static_assert(
  [] {
      Ctl::MovingAverage<std::int32_t, 2> m{};
      m.push(0x7FFFFFFF);
      m.push(0x7FFFFFFF);
      return m.average<std::int64_t>() == 0x7FFFFFFF;
  }(),
  "integral samples do not overflow the sum");

// ---- Ema / EmaDiv ------------------------------------------------------------------------------
static_assert(
  [] {
      Ctl::Ema<float> e{0.5f};
      bool            ok = !e.value();
      e.push(10.0f);
      ok = ok && e.value() == 10.0f;   // seeded by the first sample
      e.push(20.0f);
      return ok && e.value() == 15.0f;
  }(),
  "Ema: the first sample seeds, then alpha of the way");
static_assert(
  [] {
      Ctl::EmaDiv<std::int32_t> e{16};
      e.push(0);
      for(int i = 0; i < 100; ++i) { e.push(15); }
      return e.value() == 0;
  }(),
  "EmaDiv{16} fed 15 from 0 stays at 0: the dead band of a truncating division");
static_assert([] {
    Ctl::EmaDiv<std::int32_t> e{4};
    e.push(0);
    e.push(100);
    return e.value() == 25;
}());

// ---- median ------------------------------------------------------------------------------------
static_assert(
  [] {
      Ctl::MedianOf<int, 5> m;
      for(auto v : {3, 1000, 4, -900, 5}) { m.push(v); }
      return m.value() == 4;
  }(),
  "outliers do not move the median");
static_assert(
  [] {
      Ctl::MedianOf<int, 3> m;
      m.push(1);
      m.push(2);
      return !m.value();
  }(),
  "empty until N have arrived");

// ---- Ramp / approach ---------------------------------------------------------------------------
static_assert(
  [] {
      Ctl::Ramp<int> r{3, 5, 0};
      r.setTarget(10);
      int  steps = 0;
      int  prev  = 0;
      bool ok    = true;
      while(!r.reached()) {
          auto const v = r.step();
          ok           = ok && v - prev <= 3 && v <= 10;
          prev         = v;
          ++steps;
      }
      ok = ok && steps == 4 && r.value() == 10;
      r.setTarget(0);
      steps = 0;
      while(!r.reached()) {
          (void)r.step();
          ++steps;
      }
      return ok && steps == 2 && r.value() == 0;
  }(),
  "Ramp reaches the target exactly, never overshoots, up and down rates apart");

constexpr float MinFadeStep{1.0f / 2048.0f};
static_assert(Ctl::approach(0.5f,
                            0.5001f,
                            0.005f,
                            MinFadeStep)
                == 0.5001f,
              "lands on the target");
static_assert(Ctl::approach(0.5f,
                            0.5f,
                            0.005f,
                            MinFadeStep)
              == 0.5f);
static_assert(Ctl::approach(0.2f,
                            0.9f,
                            1.0f,
                            MinFadeStep)
                == 0.9f,
              "rate 1: no fade");
static_assert(Ctl::approach(0.0f,
                            1.0f,
                            0.5f,
                            MinFadeStep)
              == 0.5f);
static_assert(Ctl::approach(1.0f,
                            0.0f,
                            0.5f,
                            MinFadeStep)
              == 0.5f);

// ---- interpolation -----------------------------------------------------------------------------
constexpr std::array<Ctl::Point<std::int16_t, std::int16_t>, 4> Ntc{
  {{-400, 3900}, {0, 3100}, {250, 2048}, {1000, 380}}
};
static_assert(Ctl::interpolate<Ntc>(-400) == 3900 && Ctl::interpolate<Ntc>(0) == 3100
                && Ctl::interpolate<Ntc>(250) == 2048 && Ctl::interpolate<Ntc>(1000) == 380,
              "every knot");
static_assert(Ctl::interpolate<Ntc>(-200) == 3500 && Ctl::interpolate<Ntc>(625) == 1214,
              "between knots");
static_assert(Ctl::interpolate<Ntc>(-1000) == 3900 && Ctl::interpolate<Ntc>(5000) == 380,
              "clamped outside");

constexpr std::array<Ctl::Point<float, float>, 3> Curve{
  {{0.0f, 0.0f}, {1.0f, 10.0f}, {3.0f, 30.0f}}
};
static_assert(Ctl::interpolate<Curve>(0.5f) == 5.0f && Ctl::interpolate<Curve>(2.0f) == 20.0f);

constexpr Ctl::UniformTable<float, 6> Horizontal{
  0.0f,
  10.0f,
  {100.0f, 179.0f, 157.0f, 113.0f, 102.0f, 100.0f}
};
static_assert(Horizontal(0.0f) == 100.0f && Horizontal(10.0f) == 179.0f
              && Horizontal(5.0f) == 139.5f);
static_assert(Horizontal(-5.0f) == 100.0f && Horizontal(50.0f) == 100.0f
                && Horizontal(70.0f) == 100.0f,
              "clamped at both ends");
static_assert(Horizontal(45.0f) == 101.0f);
}   // namespace

int main() {
    Kvasir::Test::test("filters at run time");
    Ctl::MedianOf<int, 3> m;
    int volatile v = 7;
    m.push(v);
    m.push(1);
    m.push(9);
    CHECK(m.value() == 7);
    return Kvasir::Test::report();
}
