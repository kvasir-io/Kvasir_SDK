#pragma once

#include "kvasir/StartUp/Resources.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>

namespace Kvasir::Multicore {

// Gives a per-core clock on the secondary core the same epoch as a clock on the primary.
//
// The case it exists for: SysTick. Each core has its own, both count the same clk_sys, so
// they never drift against each other - the only difference is the instant each was
// started. One rendezvous at launch fixes that for the life of the program.
//
//   using Core1Systick = Kvasir::Systick::SystickClockBase<Core1SystickConfig>;   // synchronised = true
//   using Core1 = SecondaryCore<&core1Main, Core1Systick,
//                               Kvasir::Multicore::ClockSync<Core0Systick, Core1Systick>, ...>;
//
// Reference: the primary core's clock, whose Startup entry must precede the SecondaryCore
// in core 0's list (its counter has to be running when primarySync() runs). Target: the
// secondary core's clock, which provides `raw()` and `syncTo(referenceAt, rawAt)`. Both
// are read only on their own core; a SysTick clock read from the other core is silently
// wrong, since the registers are banked at the same address.
//
// Protocol (the SecondaryCore rendezvous hooks), `Rounds` times: core 1 announces itself,
// core 0 reads its clock (t0) and raises a flag, core 1 reads its raw counter the moment
// it sees the flag and answers, core 0 reads its clock again (t1). Core 1's reading lies
// between t0 and t1, so pairing it with (t0 + t1) / 2 is right to within (t1 - t0) / 2.
// The round with the smallest bracket wins. Repeating is not paranoia: both cores execute
// from the same flash through one XIP cache, and a cold fetch on either side between flag
// and read costs microseconds - one round measured 1.7 us off, the best of eight is a few
// tens of nanoseconds.
template<typename Reference, typename Target>
struct ClockSync {
    // Startup: the Reference must be in core 0's list (and before the SecondaryCore, so it
    // is counting at launch), the Target in this list - a SysTick read from the other core
    // is silently that core's.
    using Claims = brigand::list<Startup::ListedOn<Reference, 0>, Startup::ListedHere<Target>>;

    static constexpr int Rounds = 8;
    // Both sides bounded on their own clock: core 0 on Reference, core 1 on Target's raw
    // counter, which is running by the time secondarySync() is reached (its Startup entry
    // precedes this one). Generous, because core 1 may be bringing a display up first.
    static constexpr auto PrimaryTimeout   = std::chrono::seconds{2};
    static constexpr auto SecondaryTimeout = std::chrono::seconds{2};

    // Per round r the phase runs 4r+1 (core 1 arrived), 4r+2 (core 0 stamped), 4r+3
    // (core 1 answered), 4r+4 (core 0 closed the round: only then may core 1 open the
    // next, or core 0 could miss the answer). Final follows the last close.
    static constexpr std::uint32_t Final = 4U * Rounds + 1U;

    // Core 0, before the bootrom handshake: nothing else can be touching this.
    static void primaryPrepare() {
        phase.store(0, std::memory_order_relaxed);
        met.store(false, std::memory_order_relaxed);
    }

    // Core 0, while core 1 runs its Startup list.
    [[nodiscard]] static bool primarySync() {
        auto const deadline = Reference::now() + PrimaryTimeout;
        auto const waitFor  = [&](std::uint32_t value) {
            while(phase.load(std::memory_order_acquire) != value) {
                if(Reference::now() > deadline) { return false; }
            }
            return true;
        };

        typename Reference::rep bestWidth = -1;
        for(std::uint32_t r = 0; r < static_cast<std::uint32_t>(Rounds); ++r) {
            if(!waitFor(4U * r + 1U)) { return false; }
            auto const t0 = Reference::now().time_since_epoch().count();
            phase.store(4U * r + 2U, std::memory_order_release);
            if(!waitFor(4U * r + 3U)) { return false; }
            auto const t1    = Reference::now().time_since_epoch().count();
            auto const width = t1 - t0;
            if(bestWidth < 0 || width < bestWidth) {
                bestWidth = width;
                bestRound = r;
                refMid    = t0 + width / 2;
            }
            phase.store(4U * r + 4U, std::memory_order_release);
        }
        halfWidth = bestWidth / 2;
        phase.store(Final, std::memory_order_release);
        return true;
    }

    // Core 1, after its runtime inits: its clock is counting by now.
    [[nodiscard]] static bool secondarySync() {
        auto const deadline
          = Target::raw() + std::chrono::duration_cast<typename Target::duration>(SecondaryTimeout);
        auto const waitFor = [&](std::uint32_t value) {
            while(phase.load(std::memory_order_acquire) != value) {
                if(Target::raw() > deadline) { return false; }
            }
            return true;
        };

        for(std::uint32_t r = 0; r < static_cast<std::uint32_t>(Rounds); ++r) {
            if(r != 0 && !waitFor(4U * r)) { return false; }   // previous round closed
            phase.store(4U * r + 1U, std::memory_order_release);
            if(!waitFor(4U * r + 2U)) { return false; }
            raw[r] = Target::raw().count();
            phase.store(4U * r + 3U, std::memory_order_release);
        }
        if(!waitFor(Final)) { return false; }
        Target::syncTo(std::chrono::duration_cast<typename Target::duration>(
                         typename Reference::duration{refMid}),
                       typename Target::duration{raw[bestRound]});
        met.store(true, std::memory_order_release);
        return true;
    }

    // Whether the last rendezvous completed, readable from either core.
    [[nodiscard]] static bool synchronised() { return met.load(std::memory_order_acquire); }

    // The uncertainty of the adopted epoch, in Reference ticks: half the winning bracket.
    [[nodiscard]] static typename Reference::rep uncertainty() { return halfWidth; }

private:
    static inline std::atomic<std::uint32_t>               phase{0};
    static inline std::atomic<bool>                        met{false};
    static inline std::array<typename Target::rep, Rounds> raw{};
    static inline typename Reference::rep                  refMid{};
    static inline typename Reference::rep                  halfWidth{};
    static inline std::uint32_t                            bestRound{};
};

}   // namespace Kvasir::Multicore
