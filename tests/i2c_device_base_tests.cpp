// Tests for Kvasir::I2CDeviceBase: absent-device detection after consecutive NAKs, the
// probe schedule while parked, unparking on the first ACK, and config compatibility.
#include "kvasir_test.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <print>
#include <span>
#include <vector>

// The device base logs through uc_log, which is not on the test include path;
// the stubs must be defined before the driver header is parsed, so keep this
// order out of reach of include sorting.
// clang-format off
#include "support/LogStubs.hpp"
#include "kvasir/Devices/I2CDeviceBase.hpp"
// clang-format on

using namespace Kvasir::Test;
using namespace std::chrono_literals;

// The clock and the bus are the shared harness (support/): FakeClock counts
// microseconds, FakeI2C keeps every request pending until the test completes it.
#include "support/FakeI2C.hpp"

using ms = std::chrono::milliseconds;

static void setTime(ms t) { FakeClock::set(t); }

template<typename Config = Kvasir::I2CDeviceDefaults>
struct Device : Kvasir::I2CDeviceBase<FakeI2C, FakeClock, Device<Config>, 5, Config> {
    using Base = Kvasir::I2CDeviceBase<FakeI2C, FakeClock, Device<Config>, 5, Config>;

    Device() : Base{0x4b} {}

    int resets{};
    int idles{};
    int refused{};

    void resetLogic() { ++resets; }

    // a driver that polls: one read per idle tick
    void idleLogic() {
        ++idles;
        if(!this->submitRead(buf_, buf_, [](FakeI2C::Result) {})) { ++refused; }
    }

private:
    std::array<std::byte, 1> buf_{};
};

static void setup() {
    FakeI2C::reset();
    Log::warnings = 0;
    Log::infos    = 0;
    setTime(0ms);
}

// run one tick and complete whatever it submitted with `r`
template<typename D>
static void tick(D&              d,
                 FakeI2C::Result r) {
    d.handler();
    while(!FakeI2C::pending.empty()) { FakeI2C::complete(r); }
}

static void threeNaksParkTheDevice() {
    test("threeNaksParkTheDevice");
    setup();
    Device<> d;

    for(int i = 0; i < 3; ++i) { tick(d, FakeI2C::Result::notAcknowledged); }
    CHECK(d.present());
    CHECK_EQ(FakeI2C::submitted, 3);

    d.handler();   // sees the third NAK: park
    CHECK(!d.present());
    CHECK_EQ(Log::warnings, 1);
    CHECK_EQ(d.resets, 1);
    CHECK_EQ(FakeI2C::submitted, 3);   // no request in the parking tick

    auto const idles = d.idles;
    setTime(500ms);
    d.handler();
    CHECK_EQ(d.idles, idles);   // parked: no idleLogic()
    CHECK_EQ(FakeI2C::submitted, 3);
    CHECK(!d.submitSend({}, [](FakeI2C::Result) {}));   // direct submits are refused too
}

static void probeSchedule() {
    test("probeSchedule");
    setup();
    Device<> d;

    for(int i = 0; i < 3; ++i) { tick(d, FakeI2C::Result::notAcknowledged); }
    d.handler();   // parked at t=0
    CHECK(!d.present());

    // one request per probe at 1, 2, 4, 8, 16, 32, 62 s (gap doubles to the 30 s cap)
    std::vector<ms> probes;
    for(ms t{100}; t <= 100'000ms; t += 100ms) {
        setTime(t);
        auto const before = FakeI2C::submitted;
        tick(d, FakeI2C::Result::notAcknowledged);
        if(FakeI2C::submitted != before) {
            CHECK_EQ(FakeI2C::submitted - before, 1);
            probes.push_back(t);
        }
    }
    CHECK(probes
          == std::vector<ms>{1000ms, 2000ms, 4000ms, 8000ms, 16000ms, 32000ms, 62000ms, 92000ms});
    CHECK(!d.present());
    CHECK_EQ(Log::warnings, 1);   // still the single "not responding"
}

static void ackUnparksAtOnce() {
    test("ackUnparksAtOnce");
    setup();
    Device<> d;

    for(int i = 0; i < 3; ++i) { tick(d, FakeI2C::Result::notAcknowledged); }
    d.handler();
    setTime(1000ms);
    d.handler();   // probe armed and submitted by idleLogic()
    CHECK_EQ(FakeI2C::pending.size(), 1U);
    FakeI2C::complete(FakeI2C::Result::succeeded);
    CHECK(d.present());   // cleared in the callback, before handler() runs again

    auto const submitted = FakeI2C::submitted;
    setTime(1100ms);
    tick(d, FakeI2C::Result::succeeded);
    CHECK_EQ(Log::infos, 1);                       // "present again", once
    CHECK_EQ(FakeI2C::submitted, submitted + 1);   // normal polling resumed
    setTime(1200ms);
    tick(d, FakeI2C::Result::succeeded);
    CHECK_EQ(Log::infos, 1);
    CHECK_EQ(d.consecutiveNaks(), 0U);
}

static void busFaultsDoNotPark() {
    test("busFaultsDoNotPark");
    setup();
    Device<> d;

    for(int i = 0; i < 10; ++i) { tick(d, FakeI2C::Result::failed); }
    CHECK(d.present());
    CHECK_EQ(Log::warnings, 0);
    CHECK(d.resets >= 1);   // the error threshold path still runs

    // a NAK streak interrupted by a bus fault keeps counting
    Device<> e;
    tick(e, FakeI2C::Result::notAcknowledged);
    tick(e, FakeI2C::Result::failed);
    tick(e, FakeI2C::Result::notAcknowledged);
    tick(e, FakeI2C::Result::notAcknowledged);
    e.handler();
    CHECK(!e.present());
}

struct MinimalConfig {
    static constexpr auto          inFlightTimeout    = std::chrono::seconds{2};
    static constexpr std::uint32_t timeoutLogInterval = 0;
};

struct DisabledConfig : Kvasir::I2CDeviceDefaults {
    static constexpr std::uint8_t absentAfterNaks = 0;
};

static void configCompatibility() {
    test("configCompatibility");
    setup();

    Device<MinimalConfig> d;   // no absent* members: defaults apply
    static_assert(Device<MinimalConfig>::AbsentAfterNaks == 3);
    static_assert(Device<MinimalConfig>::ProbeInterval == 1000ms);
    for(int i = 0; i < 3; ++i) { tick(d, FakeI2C::Result::notAcknowledged); }
    d.handler();
    CHECK(!d.present());

    Device<DisabledConfig> e;
    for(int i = 0; i < 20; ++i) { tick(e, FakeI2C::Result::notAcknowledged); }
    e.handler();
    CHECK(e.present());
    CHECK_EQ(Log::warnings, 1);   // only d's
}

static void flappingDeviceLogsAreLimited() {
    test("flappingDeviceLogsAreLimited");
    setup();
    Device<> d;

    // answers exactly one probe, then NAKs again -- 20 times within 5 s
    ms t{0};
    for(int flap = 0; flap < 20; ++flap) {
        for(int i = 0; i < 3; ++i) {
            setTime(t += 10ms);
            tick(d, FakeI2C::Result::notAcknowledged);
        }
        setTime(t += 10ms);
        d.handler();   // park
        setTime(t += 1000ms);
        tick(d, FakeI2C::Result::succeeded);   // the probe succeeds
        setTime(t += 10ms);
        d.handler();   // reports "present again"
    }
    CHECK(Log::warnings + Log::infos < 40);
    CHECK(Log::warnings + Log::infos <= 12);
}

int main() {
    threeNaksParkTheDevice();
    probeSchedule();
    ackUnparksAtOnce();
    busFaultsDoNotPark();
    configCompatibility();
    flappingDeviceLogsAreLimited();

    if(failures == 0) {
        std::print("all i2c device base tests passed\n");
    } else {
        std::print("{} failure(s)\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
