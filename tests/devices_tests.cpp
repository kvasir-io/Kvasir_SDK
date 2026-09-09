// Tests for the Kvasir::Devices layer: the SM::match helper, SharedBusDevice,
// BusPowerManager and I2CDeviceBase.
//
// BusPowerManager and I2CDeviceBase log through UC_LOG_W/UC_LOG_I but do not include uc_log
// themselves, so the test supplies a stub before including them. Everything else is driven
// with fakes: a controllable Clock, a fake shared bus, a fake I2C peripheral and a small
// fake chip file so that the power pin resolves to real register actions.
#include <string>
#include <vector>

// stub for the uc_log macro the device headers use without including uc_log. Only the
// format string is recorded, the arguments are discarded.
namespace TestLog {
inline std::vector<std::string> messages;

template<typename... Ts>
void record(char const* format,
            Ts&&...) {
    messages.emplace_back(format);
}
}   // namespace TestLog

#define UC_LOG_W(...) ::TestLog::record(__VA_ARGS__)
#define UC_LOG_I(...) ::TestLog::record(__VA_ARGS__)

// RateLimiter.hpp only defines KVASIR_LOG_LIMITED when uc_log is available; the stub
// keeps the rate limiting but drops the suppressed-count argument.
#define KVASIR_LOG_LIMITED(decision, LOG, ...)                            \
    do {                                                                  \
        if(auto const kvasir_limited_ = (decision)) { LOG(__VA_ARGS__); } \
    } while(false)

#include "kvasir/Devices/BusPowerManager.hpp"
#include "kvasir/Devices/I2CDeviceBase.hpp"
#include "kvasir/Devices/SharedBusDevice.hpp"
#include "kvasir/Devices/utils.hpp"
#include "kvasir/Io/PinFactories.hpp"
#include "kvasir/Util/StaticFunction.hpp"
#include "kvasir_test.hpp"
#include "support/FakeClock.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <variant>

using namespace Kvasir::Test;
using W = Recorder::Write;
using R = Recorder::Read;

namespace {

// ---------------------------------------------------------------------------
// a clock whose current time the test drives by hand, in milliseconds (support/)
using MsClock = Kvasir::Test::FakeClockT<std::chrono::milliseconds>;

// ---------------------------------------------------------------------------
// fake chip file for a single power pin at port 0, pin 0
constexpr unsigned PowerPortAddress = 0x300;

using PowerAddr
  = Kvasir::Register::Address<PowerPortAddress, 0x00000000, 0x00000000, std::uint32_t>;

template<int Pin>
using PowerField = Kvasir::Register::
  FieldLocation<PowerAddr, (1U << unsigned(Pin)), Kvasir::Register::ReadWriteAccess, std::uint32_t>;

constexpr unsigned PowerDirAddress = 0x304;

using PowerDirAddr
  = Kvasir::Register::Address<PowerDirAddress, 0x00000000, 0x00000000, std::uint32_t>;

template<int Pin>
using PowerDirField = Kvasir::Register::FieldLocation<PowerDirAddr,
                                                      (1U << unsigned(Pin)),
                                                      Kvasir::Register::ReadWriteAccess,
                                                      std::uint32_t>;

}   // namespace

namespace Kvasir { namespace Io {

    template<OutputType OT, OutputSpeed OS, OutputInit OI, int Pin>
    struct MakeAction<Action::Output<OT, OS, OI>, Register::PinLocation<0, Pin>>
      : Register::Detail::Set<PowerDirField<Pin>> {};

    template<int Pin>
    struct MakeAction<Action::Set, Register::PinLocation<0, Pin>>
      : Register::Detail::Set<PowerField<Pin>> {};

    template<int Pin>
    struct MakeAction<Action::Clear, Register::PinLocation<0, Pin>>
      : Register::Detail::Clear<PowerField<Pin>> {};

}}   // namespace Kvasir::Io

namespace {

using PowerPin = Kvasir::Register::PinLocation<0, 0>;

// ---------------------------------------------------------------------------
// a fake shared bus with a settable acquire result
struct FakeSharedBus {
    static inline bool available    = true;
    static inline int  acquireCalls = 0;
    static inline int  releaseCalls = 0;

    static void reset() {
        available    = true;
        acquireCalls = 0;
        releaseCalls = 0;
    }

    static bool acquire() {
        ++acquireCalls;
        return available;
    }

    static void release() { ++releaseCalls; }
};

// ---------------------------------------------------------------------------
// a device for the BusPowerManager to drive
struct FakeDevice {
    int  handlerCalls    = 0;
    int  resetCalls      = 0;
    bool wantsPowerReset = false;

    void handler() { ++handlerCalls; }

    bool needsPowerReset() const { return wantsPowerReset; }

    void reset() {
        ++resetCalls;
        wantsPowerReset = false;
    }
};

struct FakeBus {};

}   // namespace

// ===========================================================================
// Devices/utils.hpp: overloaded / match
// ===========================================================================

static void matchVisitsTheActiveAlternative() {
    test("matchVisitsTheActiveAlternative");

    using V = std::variant<int, double, char const*>;

    auto describe = [](V const& v) {
        return Kvasir::SM::match(
          v,
          [](int) { return 1; },
          [](double) { return 2; },
          [](char const*) { return 3; });
    };

    CHECK_EQ(describe(V{42}), 1);
    CHECK_EQ(describe(V{4.2}), 2);
    CHECK_EQ(describe(V{"hello"}), 3);
}

static void matchCanMutateAndReturnVoid() {
    test("matchCanMutateAndReturnVoid");

    std::variant<int, double> v{1};
    int                       seen = 0;

    Kvasir::SM::match(
      v,
      [&seen](int& x) {
          seen = x;
          x    = 99;
      },
      [](double&) {});

    CHECK_EQ(seen, 1);
    CHECK_EQ(std::get<int>(v), 99);

    // a generic matcher covers every alternative at once
    int visited = 0;
    Kvasir::SM::match(v, [&visited](auto const&) { ++visited; });
    CHECK_EQ(visited, 1);
}

// ===========================================================================
// SharedBusDevice
// ===========================================================================

static void sharedBusAcquireAndRelease() {
    test("sharedBusAcquireAndRelease");
    FakeSharedBus::reset();

    Kvasir::SharedBusDevice<FakeSharedBus> device;
    CHECK(!device.isOwner());

    CHECK(device.acquire());
    CHECK(device.isOwner());
    CHECK_EQ(FakeSharedBus::acquireCalls, 1);

    // acquiring again while already the owner does not touch the bus
    CHECK(device.acquire());
    CHECK_EQ(FakeSharedBus::acquireCalls, 1);

    device.release();
    CHECK(!device.isOwner());
    CHECK_EQ(FakeSharedBus::releaseCalls, 1);

    // releasing again is a no-op
    device.release();
    CHECK_EQ(FakeSharedBus::releaseCalls, 1);
}

static void sharedBusAcquireFails() {
    test("sharedBusAcquireFails");
    FakeSharedBus::reset();
    FakeSharedBus::available = false;

    Kvasir::SharedBusDevice<FakeSharedBus> device;
    CHECK(!device.acquire());
    CHECK(!device.isOwner());
    CHECK_EQ(FakeSharedBus::acquireCalls, 1);

    // a device that never became the owner must not release the bus
    device.release();
    CHECK_EQ(FakeSharedBus::releaseCalls, 0);

    // once the bus frees up the next attempt succeeds
    FakeSharedBus::available = true;
    CHECK(device.acquire());
    CHECK(device.isOwner());
}

static void sharedBusErrorAndResetCounts() {
    test("sharedBusErrorAndResetCounts");
    FakeSharedBus::reset();

    Kvasir::SharedBusDevice<FakeSharedBus> device;

    // reset_count starts at 1, so a fresh device does not want a power reset
    CHECK(!device.needsPowerReset());

    // reset() zeroes the counter and drops bus ownership
    CHECK(device.acquire());
    device.reset();
    CHECK(!device.isOwner());
    CHECK_EQ(FakeSharedBus::releaseCalls, 1);
    CHECK(!device.needsPowerReset());

    device.incrementErrorCount();
    device.incrementErrorCount();
    CHECK_EQ(device.error_count, 2U);

    device.resetErrorCount();
    CHECK_EQ(device.error_count, 0U);
    CHECK_EQ(device.reset_count, 1U);
}

static void sharedBusResetHandlerOnErrors() {
    test("sharedBusResetHandlerOnErrors");
    FakeSharedBus::reset();

    Kvasir::SharedBusDevice<FakeSharedBus> device;

    bool flag         = false;
    int  callbackHits = 0;
    auto callback     = [&callbackHits] { ++callbackHits; };

    // below the threshold nothing happens
    for(int i = 0; i != 5; ++i) {
        device.incrementErrorCount();
        CHECK(!device.resetHandler(flag, callback));
    }
    CHECK(!flag);
    CHECK_EQ(callbackHits, 0);

    // the sixth error trips it: error_count > 5
    device.incrementErrorCount();
    CHECK(device.resetHandler(flag, callback));
    CHECK(flag);
    CHECK_EQ(callbackHits, 1);
    CHECK_EQ(device.error_count, 0U);
    CHECK_EQ(device.reset_count, 2U);
}

static void sharedBusResetHandlerAfterReset() {
    test("sharedBusResetHandlerAfterReset");
    FakeSharedBus::reset();

    Kvasir::SharedBusDevice<FakeSharedBus> device;

    // reset() sets reset_count to 0, which makes the next resetHandler fire once
    device.reset();
    CHECK_EQ(device.reset_count, 0U);

    bool flag = false;
    CHECK(device.resetHandler(flag));
    CHECK(flag);
    CHECK_EQ(device.reset_count, 1U);

    // and not again afterwards
    bool second = false;
    CHECK(!device.resetHandler(second));
    CHECK(!second);
}

static void sharedBusResetHandlerResetterKinds() {
    test("sharedBusResetHandlerResetterKinds");
    FakeSharedBus::reset();

    // resetHandler accepts a bool to set, an object with reset(), or a callable
    struct Resettable {
        int calls = 0;

        void reset() { ++calls; }
    };

    Kvasir::SharedBusDevice<FakeSharedBus> device;
    device.reset();   // arm the reset_count == 0 path

    bool       flag = false;
    Resettable resettable;
    int        callableHits = 0;

    CHECK(device.resetHandler(flag, resettable, [&callableHits] { ++callableHits; }));
    CHECK(flag);
    CHECK_EQ(resettable.calls, 1);
    CHECK_EQ(callableHits, 1);
}

static void sharedBusNeedsPowerResetAfterRepeatedResets() {
    test("sharedBusNeedsPowerResetAfterRepeatedResets");
    FakeSharedBus::reset();

    Kvasir::SharedBusDevice<FakeSharedBus> device;
    bool                                   flag = false;

    // every trip of the error threshold bumps reset_count; after enough of them the device
    // asks for a power cycle
    for(int round = 0; round != 5; ++round) {
        for(int i = 0; i != 6; ++i) { device.incrementErrorCount(); }
        CHECK(device.resetHandler(flag));
    }

    CHECK_EQ(device.reset_count, 6U);
    CHECK(device.needsPowerReset());

    // resetErrorCount puts reset_count back to 1 and clears the request
    device.resetErrorCount();
    CHECK(!device.needsPowerReset());
}

// ===========================================================================
// BusPowerManager
// ===========================================================================

using Manager         = Kvasir::BusPowerManager<FakeBus, MsClock, PowerPin, false, FakeDevice>;
using InvertedManager = Kvasir::BusPowerManager<FakeBus, MsClock, PowerPin, true, FakeDevice>;

static void busPowerManagerPowerOnSequence() {
    test("busPowerManagerPowerOnSequence");
    MsClock::reset();
    TestLog::messages.clear();

    FakeDevice device;
    Manager    manager{device};

    // first handler call: configure the pin as an output and drive it low
    manager.handler();
    CHECK_EQ(writeCount(PowerDirAddress), 1U);
    CHECK_EQ(writtenValue(PowerDirAddress), 0x1U);
    CHECK_EQ(writeCount(PowerPortAddress), 1U);
    CHECK_EQ(writtenValue(PowerPortAddress), 0x0U);
    CHECK_EQ(device.handlerCalls, 0);

    // nothing happens until the 500ms power-off time has elapsed
    recorder.reset();
    MsClock::advance(std::chrono::milliseconds{499});
    manager.handler();
    checkActions({});

    // once it has, the pin is driven high
    recorder.reset();
    MsClock::advance(std::chrono::milliseconds{2});
    manager.handler();
    CHECK_EQ(writeCount(PowerPortAddress), 1U);
    CHECK_EQ(writtenValue(PowerPortAddress), 0x1U);

    // then a 100ms settling time before the devices are serviced
    recorder.reset();
    MsClock::advance(std::chrono::milliseconds{99});
    manager.handler();
    CHECK_EQ(device.handlerCalls, 0);

    MsClock::advance(std::chrono::milliseconds{2});
    manager.handler();   // transitions to idle, does not yet call the device
    CHECK_EQ(device.handlerCalls, 0);

    manager.handler();   // now in idle
    CHECK_EQ(device.handlerCalls, 1);

    manager.handler();
    CHECK_EQ(device.handlerCalls, 2);
}

static void busPowerManagerInvertedDrivesPinTheOtherWay() {
    test("busPowerManagerInvertedDrivesPinTheOtherWay");
    MsClock::reset();
    TestLog::messages.clear();

    FakeDevice      device;
    InvertedManager manager{device};

    // with invert the "off" state drives the pin high
    manager.handler();
    CHECK_EQ(writtenValue(PowerPortAddress), 0x1U);

    recorder.reset();
    recorder.setReadValue(PowerPortAddress, 0xFFFFFFFF);
    MsClock::advance(std::chrono::milliseconds{501});
    manager.handler();
    // ... and the "on" state drives it low
    CHECK_EQ(writtenValue(PowerPortAddress) & 0x1U, 0x0U);
}

static void busPowerManagerRestartsOnDeviceRequest() {
    test("busPowerManagerRestartsOnDeviceRequest");
    MsClock::reset();
    TestLog::messages.clear();

    FakeDevice device;
    Manager    manager{device};

    // run the state machine up to idle
    manager.handler();
    MsClock::advance(std::chrono::milliseconds{501});
    manager.handler();
    MsClock::advance(std::chrono::milliseconds{101});
    manager.handler();
    manager.handler();
    CHECK_EQ(device.handlerCalls, 1);

    // the device now asks for a power cycle
    device.wantsPowerReset = true;
    recorder.reset();
    manager.handler();

    CHECK_EQ(device.handlerCalls, 2);
    CHECK_EQ(device.resetCalls, 1);
    CHECK_EQ(TestLog::messages.size(), 1U);

    // the next handler call starts the power-off sequence again
    recorder.reset();
    manager.handler();
    CHECK_EQ(writeCount(PowerDirAddress), 1U);
    CHECK_EQ(writtenValue(PowerPortAddress), 0x0U);
}

static void busPowerManagerDrivesSeveralDevices() {
    test("busPowerManagerDrivesSeveralDevices");
    MsClock::reset();
    TestLog::messages.clear();

    using MultiManager
      = Kvasir::BusPowerManager<FakeBus, MsClock, PowerPin, false, FakeDevice, FakeDevice>;

    FakeDevice   a;
    FakeDevice   b;
    MultiManager manager{a, b};

    manager.handler();
    MsClock::advance(std::chrono::milliseconds{501});
    manager.handler();
    MsClock::advance(std::chrono::milliseconds{101});
    manager.handler();
    manager.handler();

    // every device gets its handler called
    CHECK_EQ(a.handlerCalls, 1);
    CHECK_EQ(b.handlerCalls, 1);

    // a request from either device resets all of them
    b.wantsPowerReset = true;
    manager.handler();
    CHECK_EQ(a.resetCalls, 1);
    CHECK_EQ(b.resetCalls, 1);
}

static void makeBusPowerManagerDeducesDevices() {
    test("makeBusPowerManagerDeducesDevices");
    MsClock::reset();

    FakeDevice device;
    auto       manager = Kvasir::make_BusPowerManager<FakeBus, MsClock, PowerPin, false>(device);

    manager.handler();
    CHECK_EQ(writeCount(PowerDirAddress), 1U);
}

// ===========================================================================
// I2CDeviceBase
// ===========================================================================

namespace {

struct FakeI2C {
    enum class Result { ok, failed, notAcknowledged };

    struct Request {
        std::uint8_t                             address{};
        std::span<std::byte const>               sendData{};
        std::span<std::byte>                     receiveData{};
        Kvasir::StaticFunction<void(Result), 32> callback{};
    };

    static inline bool accept      = true;
    static inline int  submitCalls = 0;

    // held in a function local static: a static inline data member would need Request's
    // default member initializers before FakeI2C itself is complete
    static Request& lastRequest() {
        static Request request{};
        return request;
    }

    static void reset() {
        accept        = true;
        submitCalls   = 0;
        lastRequest() = Request{};
    }

    static bool submit(Request const& r) {
        ++submitCalls;
        if(!accept) { return false; }
        lastRequest() = r;
        return true;
    }

    // completes the outstanding request the way the peripheral would
    static void complete(Result r) { lastRequest().callback(r); }
};

struct FakeI2CDevice : Kvasir::I2CDeviceBase<FakeI2C, MsClock, FakeI2CDevice> {
    using base = Kvasir::I2CDeviceBase<FakeI2C, MsClock, FakeI2CDevice>;

    int idleCalls  = 0;
    int resetCalls = 0;

    constexpr explicit FakeI2CDevice(std::uint8_t address) : base{address} {}

    void idleLogic() { ++idleCalls; }

    void resetLogic() { ++resetCalls; }
};

}   // namespace

static void i2cSubmitSend() {
    test("i2cSubmitSend");
    MsClock::reset();
    FakeI2C::reset();

    FakeI2CDevice device{0x42};

    std::array<std::byte, 2> const payload{std::byte{0xAB}, std::byte{0xCD}};

    int             completions = 0;
    FakeI2C::Result seen{};
    CHECK(device.submitSend(payload, [&](FakeI2C::Result r) {
        ++completions;
        seen = r;
    }));

    CHECK_EQ(FakeI2C::submitCalls, 1);
    CHECK_EQ(FakeI2C::lastRequest().address, 0x42U);
    CHECK_EQ(FakeI2C::lastRequest().sendData.size(), 2U);
    CHECK_EQ(FakeI2C::lastRequest().receiveData.size(), 0U);

    // while the request is in flight the handler does nothing
    device.handler();
    CHECK_EQ(device.idleCalls, 0);

    FakeI2C::complete(FakeI2C::Result::ok);
    CHECK_EQ(completions, 1);
    CHECK(seen == FakeI2C::Result::ok);

    // once it completed the handler runs the idle logic again
    device.handler();
    CHECK_EQ(device.idleCalls, 1);
}

static void i2cSubmitReadAndReceive() {
    test("i2cSubmitReadAndReceive");
    MsClock::reset();
    FakeI2C::reset();

    FakeI2CDevice device{0x10};

    std::array<std::byte, 1> const send{std::byte{0x01}};
    std::array<std::byte, 4>       recv{};

    CHECK(device.submitRead(send, recv, [](FakeI2C::Result) {}));
    CHECK_EQ(FakeI2C::lastRequest().sendData.size(), 1U);
    CHECK_EQ(FakeI2C::lastRequest().receiveData.size(), 4U);
    FakeI2C::complete(FakeI2C::Result::ok);

    CHECK(device.submitReceive(recv, [](FakeI2C::Result) {}));
    CHECK_EQ(FakeI2C::lastRequest().sendData.size(), 0U);
    CHECK_EQ(FakeI2C::lastRequest().receiveData.size(), 4U);
    FakeI2C::complete(FakeI2C::Result::ok);
}

static void i2cSubmitRejected() {
    test("i2cSubmitRejected");
    MsClock::reset();
    FakeI2C::reset();
    FakeI2C::accept = false;

    FakeI2CDevice device{0x10};

    std::array<std::byte, 1> const send{std::byte{0x01}};
    CHECK(!device.submitSend(send, [](FakeI2C::Result) {}));

    // the device did not go in flight, so the handler keeps running the idle logic
    device.handler();
    CHECK_EQ(device.idleCalls, 1);
}

static void i2cErrorsTriggerReset() {
    test("i2cErrorsTriggerReset");
    MsClock::reset();
    FakeI2C::reset();

    FakeI2CDevice device{0x10};

    std::array<std::byte, 1> const send{std::byte{0x01}};

    // a failed transfer increments the error count through the callback wrapper. Only
    // failures are used here: consecutive NAKs would park the device as absent before the
    // error threshold is reached (that path is covered in i2c_device_base_tests.cpp).
    for(int i = 0; i != 6; ++i) {
        CHECK(device.submitSend(send, [](FakeI2C::Result) {}));
        FakeI2C::complete(FakeI2C::Result::failed);
    }

    CHECK_EQ(device.resetCalls, 0);

    // the threshold is "> 5", so the sixth error trips it on the next handler call
    device.handler();
    CHECK_EQ(device.resetCalls, 1);
    CHECK_EQ(device.idleCalls, 1);

    // the count was cleared, so the following handler call does not reset again
    device.handler();
    CHECK_EQ(device.resetCalls, 1);
    CHECK_EQ(device.idleCalls, 2);
}

static void i2cSuccessfulTransfersDoNotCount() {
    test("i2cSuccessfulTransfersDoNotCount");
    MsClock::reset();
    FakeI2C::reset();

    FakeI2CDevice device{0x10};

    std::array<std::byte, 1> const send{std::byte{0x01}};
    for(int i = 0; i != 10; ++i) {
        CHECK(device.submitSend(send, [](FakeI2C::Result) {}));
        FakeI2C::complete(FakeI2C::Result::ok);
    }

    device.handler();
    CHECK_EQ(device.resetCalls, 0);
    CHECK_EQ(device.idleCalls, 1);
}

static void i2cInFlightWatchdog() {
    test("i2cInFlightWatchdog");
    MsClock::reset();
    FakeI2C::reset();
    TestLog::messages.clear();

    FakeI2CDevice device{0x33};

    std::array<std::byte, 1> const send{std::byte{0x01}};
    CHECK(device.submitSend(send, [](FakeI2C::Result) {}));

    // just under the two second timeout nothing happens
    MsClock::advance(std::chrono::milliseconds{1999});
    device.handler();
    CHECK_EQ(device.resetCalls, 0);
    CHECK_EQ(device.idleCalls, 0);
    CHECK(TestLog::messages.empty());

    // past it the watchdog forces a reset and logs
    MsClock::advance(std::chrono::milliseconds{2});
    device.handler();
    CHECK_EQ(device.resetCalls, 1);
    CHECK_EQ(TestLog::messages.size(), 1U);

    // the device is no longer in flight, so normal servicing resumes
    device.handler();
    CHECK_EQ(device.idleCalls, 1);
}

int main() {
    matchVisitsTheActiveAlternative();
    matchCanMutateAndReturnVoid();

    sharedBusAcquireAndRelease();
    sharedBusAcquireFails();
    sharedBusErrorAndResetCounts();
    sharedBusResetHandlerOnErrors();
    sharedBusResetHandlerAfterReset();
    sharedBusResetHandlerResetterKinds();
    sharedBusNeedsPowerResetAfterRepeatedResets();

    busPowerManagerPowerOnSequence();
    busPowerManagerInvertedDrivesPinTheOtherWay();
    busPowerManagerRestartsOnDeviceRequest();
    busPowerManagerDrivesSeveralDevices();
    makeBusPowerManagerDeducesDevices();

    i2cSubmitSend();
    i2cSubmitReadAndReceive();
    i2cSubmitRejected();
    i2cErrorsTriggerReset();
    i2cSuccessfulTransfersDoNotCount();
    i2cInFlightWatchdog();

    return Kvasir::Test::report();
}
