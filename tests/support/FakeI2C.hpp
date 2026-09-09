#pragma once

#include "FakeClock.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <vector>

/// A queued I2C bus for host tests of anything on I2CDeviceBase, and the reset line
/// that records into the same transcript.
///
/// `submit` records the request in `log` and keeps it pending; a completion runs the
/// request's callback the way the I2C interrupt would. Two ways to complete: `complete()`
/// answers every pending request from the canned register table (`regs`, keyed by the
/// register the request addressed, `regBytes` wide) with `answer` as the result, which
/// is what a driver's bring-up and data reads want; `complete(Result)` finishes the
/// oldest pending request with that result and no data, which is what a test of the
/// base's bookkeeping wants. `refuse` makes submit() return false (a full queue).
namespace Kvasir::Test {

struct Transaction {
    enum class Kind : std::uint8_t { write, read, hold, release };

    Kind                      kind{};
    std::vector<std::uint8_t> sent{};
    std::size_t               recvLen{};
    FakeClock::time_point     at{};
};

struct FakeI2C {
    enum class Result : std::uint8_t { failed, notAcknowledged, succeeded };

    struct Request {
        std::uint8_t                address{};
        std::span<std::byte const>  sendData{};
        std::span<std::byte>        receiveData{};
        std::function<void(Result)> callback{};
    };

    static inline std::vector<Transaction>                           log{};
    static inline std::vector<Request>                               pending{};
    static inline std::map<std::uint16_t, std::vector<std::uint8_t>> regs{};
    static inline std::size_t                                        regBytes{2};
    static inline Result                                             answer{Result::succeeded};
    static inline bool                                               refuse{false};
    static inline int                                                addressSeen{-1};
    static inline int                                                submitted{};

    static bool submit(Request const& r) {
        ++submitted;
        if(refuse) { return false; }
        addressSeen = r.address;
        Transaction t{};
        t.kind = r.receiveData.empty() ? Transaction::Kind::write : Transaction::Kind::read;
        for(auto const b : r.sendData) { t.sent.push_back(static_cast<std::uint8_t>(b)); }
        t.recvLen = r.receiveData.size();
        t.at      = FakeClock::now();
        log.push_back(t);
        pending.push_back(r);
        return true;
    }

    /// The register a request addresses: its first `regBytes` bytes, big-endian.
    static std::uint16_t registerOf(std::span<std::byte const> sent) {
        if(regBytes == 2 && sent.size() >= 2) {
            return static_cast<std::uint16_t>((static_cast<std::uint8_t>(sent[0]) << 8)
                                              | static_cast<std::uint8_t>(sent[1]));
        }
        return sent.empty() ? std::uint16_t{0} : static_cast<std::uint8_t>(sent[0]);
    }

    /// Every pending request answered from the register table with `answer`.
    static void complete() {
        auto batch = std::move(pending);
        pending.clear();
        for(auto& r : batch) {
            if(answer == Result::succeeded && !r.receiveData.empty()) {
                auto const it = regs.find(registerOf(r.sendData));
                for(std::size_t i = 0; i < r.receiveData.size(); ++i) {
                    r.receiveData[i] = it != regs.end() && i < it->second.size()
                                       ? static_cast<std::byte>(it->second[i])
                                       : std::byte{0};
                }
            }
            r.callback(answer);
        }
    }

    /// The oldest pending request finished with `r`, no data.
    static void complete(Result r) {
        auto req = pending.front();
        pending.erase(pending.begin());
        req.callback(r);
    }

    static void reset() {
        log.clear();
        pending.clear();
        regs.clear();
        regBytes    = 2;
        answer      = Result::succeeded;
        refuse      = false;
        addressSeen = -1;
        submitted   = 0;
    }
};

/// A reset line that writes its two edges into the bus transcript.
struct FakeResetLine {
    static void hold() {
        FakeI2C::log.push_back({Transaction::Kind::hold, {}, 0, FakeClock::now()});
    }

    static void release() {
        FakeI2C::log.push_back({Transaction::Kind::release, {}, 0, FakeClock::now()});
    }
};

}   // namespace Kvasir::Test
