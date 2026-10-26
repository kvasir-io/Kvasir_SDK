#pragma once
// A reset the firmware does to itself, announced to the debug probe first.
//
// Why: on the RP2040, a J-Link access while the chip resets itself (a watchdog reboot, the boot
// ROM's reset_to_usb_boot) fails, and SEGGER's DLL then "recovers" the chip through the Rescue
// Debug Port. That sets CHIP_RESET.PSM_RESTART_FLAG (RP2040 data sheet 2.3.4.2 "Rescue DP" and the
// CHIP_RESET register, md l.7562 / l.7892), and the boot ROM parks core 0 in its safe loop (wfi at
// 0x30) instead of booting or entering BOOTSEL. The DLL cannot be told not to, so the firmware and
// the log printer (uc_log) agree that the probe stays off the chip while it resets:
//
//   firmware                                     printer (uc_log JLinkRttReader)
//   ------------------------------------------   ------------------------------------------------
//                                                at each session start: finds `kvasir_announced_
//                                                reset` in the map file; if the magic is there,
//                                                writes armed = hostArmed
//   announce(): armed == hostArmed? else return
//   request = {stay away, sequence + 1}
//   spin until ack == request (bounded)          polls the block every few ms; on request != ack:
//                                                drains RTT, stops RTT, writes ack = request, and
//                                                closes the J-Link session (JLINK_Close)
//   spin a short grace for that close
//   return - the caller resets at once           touches nothing for ~1.5 s (or the time asked
//                                                for), then reconnects and waits for the RTT
//                                                control block of the new boot
//
// `request` is two halves: bits 15:0 a sequence number (never 0), bits 31:16 how long the printer
// is asked to stay off the chip, in 100 ms units (0: its default, 1.5 s). A reset later than that
// (a watchdog timeout left to run out) asks for longer. The printer acks the whole word.
//
// "Is a debugger there?" is the core's Core::haltingDebug() (cortex_common/Debug.hpp, which has the sources):
// `unknown` where software cannot read DHCSR (the Cortex-M0+, the Cortex-M4), so the printer says it is
// there by writing `armed`. Where the core reads DHCSR.C_DEBUGEN (the Cortex-M33) it is checked as
// well: a probe that went away and cleared it costs no wait.
//
// Placement: .data, constant-initialised, so every boot starts with {Magic, 0, 0, 0}: not armed,
// request == ack. A stale block left in RAM (a reset into BOOTSEL) costs at most one needless
// pause of a printer that reconnects to the boot ROM.
//
// Only announce() references the block: a firmware that never calls it has none (--gc-sections).
// Without USE_UC_LOG there is no RTT a printer could run a session on, so nothing can arm the
// block: announce(), printerListening() and waitForPrinter() are constants and the block is gone.
// All accesses are volatile, which keeps LTO from splitting or folding it.
//
// announce() is safe from a panic or fault handler: interrupts may be masked, no logging, no
// allocation, no clock set-up - memory and register accesses only. Its waits are timed by a free-running
// microsecond counter when the chip package provides one (kvasir_announced_reset_time_us(), a register read) and it
// runs; else, or while it stands still, by counted polls at the chip's fastest clock (Register::CpuHzCeiling<Tag::User>,
// the hook chip/WaitBounds.hpp specialises; kvasir/Port/Hooks.hpp). Interrupts are masked while it waits,
// so interrupt load cannot stretch a wait past the host's pause; a caller that goes on (not armed, timed out) had
// them masked for at most the ack wait.

#include "kvasir/Common/Tags.hpp"
#include "kvasir/Port/Capabilities.hpp"
#include "kvasir/Port/Hooks.hpp"

#include <cstdint>
#ifdef __arm__
    #include "core/InterruptMask.hpp"
#endif
#ifdef __arm__
    #include "cortex_common/Debug.hpp"   // Core::haltingDebug(): DHCSR through the core package's registers
#endif

/// A free-running microsecond counter, if the chip package has one (a weak definition there; chip_rp_common: the
/// TIMER's raw low word). Not defined: null, and announce() counts polls.
// Also declared in uc_log detail/LossLedger.hpp; the guard avoids gcc's -Wredundant-decls.
#ifndef KVASIR_ANNOUNCED_RESET_TIME_US_DECLARED
    #define KVASIR_ANNOUNCED_RESET_TIME_US_DECLARED
extern "C" [[gnu::weak]] std::uint32_t kvasir_announced_reset_time_us();
#endif

namespace Kvasir::AnnouncedReset {

/// The host's side (uc_log `printer/AnnouncedReset.hpp`) has the same numbers.
inline constexpr std::uint32_t magicValue = 0x5253'5441U;   // "ATSR" in memory: the block is there
inline constexpr std::uint32_t hostArmed  = 0x4D52'4148U;   // "HARM" in memory: a printer listens

struct Block {
    std::uint32_t magic;     // Magic, from the image
    std::uint32_t armed;     // hostArmed while a printer listens (written by the host)
    std::uint32_t request;   // {stay away in 100 ms (31:16), sequence (15:0, never 0)} - announce()
    std::uint32_t ack;       // = request once the host is off the chip (written by the host)
};

static_assert(sizeof(Block) == 16,
              "the host reads the block as four words");

enum class Outcome : std::uint8_t {
    notArmed,       // no printer listens: returned at once
    acknowledged,   // the printer is off the chip
    timedOut        // armed, but no ack within the bound (a printer that died, GDB, ...)
};

/// Loop counts, not time: announce() may run before the clocks are set up or with them broken.
struct Polls {
    std::uint32_t ack;     // polls of `ack` before giving up
    std::uint32_t grace;   // polls after the ack, for the host's JLINK_Close to finish
};

namespace detail {
    // Each poll is at least a load, a compare, the counter update and a taken branch. On the
    // Cortex-M0+ that is >= 6 cycles (DDI0484C Table 3-1: LDR 2, CMP 1, ADDS/SUBS 1, B<cc> taken
    // 2); the Cortex-M33 can dual-issue some 16-bit pairs (100230 A1.4.1 "Limited dual-issue"), so
    // >= 2 is assumed there and on the Cortex-M4. The clock is the chip's ceiling, the hook
    // Register::CpuHzCeiling<Tag::User> (RP2040 200 MHz, RP2350 266 MHz, SAM D21 48 MHz, STM32F413
    // 100 MHz - each chip's chip/WaitBounds.hpp has its source). So the bound is at least the time
    // asked for at the fastest clock and longer below it: the ack wait is >= 0.5 s at the ceiling
    // (~0.75 s at 133 MHz on the RP2040 with 6-cycle polls, ~8 s at its 12 MHz XOSC before the PLL).
    // It is spent only when a printer armed the block in this boot and does not answer.
#ifdef __ARM_ARCH_6M__
    inline constexpr std::uint64_t minCyclesPerPoll = 6;
#else
    inline constexpr std::uint64_t minCyclesPerPoll = 2;
#endif

    // The chip's ceiling, looked up where Dep is known (a chip's specialisation may come after this header). On a
    // host (no chip) the primary's 0: no polls - a host has no printer to wait for.
    template<typename... Dep>
    constexpr std::uint64_t maxCoreHz() {
        constexpr std::uint64_t hz = Port::LateT<Register::CpuHzCeiling<Tag::User>, Dep...>::value;
#ifdef __arm__
        static_assert(hz > 0,
                      "AnnouncedReset counts its waits at the chip's fastest clock: no chip "
                      "package specialised "
                      "Kvasir::Register::CpuHzCeiling<Tag::User> (chip/WaitBounds.hpp) in this "
                      "translation unit");
#endif
        return hz;
    }

    template<typename... Dep>
    constexpr std::uint32_t pollsFor(std::uint64_t microseconds) {
        return static_cast<std::uint32_t>(maxCoreHz<Dep...>() * microseconds / 1'000'000
                                          / minCyclesPerPoll);
    }

    // PRIMASK set for its lifetime, the previous value back at the end: the core's IrqGuard (core/InterruptMask.hpp).
    // Elsewhere (the host, RISC-V) nothing.
#ifdef __arm__
    using IrqMask = Kvasir::Core::IrqGuard;
#else
    struct IrqMask {
        IrqMask()                          = default;
        IrqMask(IrqMask const&)            = delete;
        IrqMask& operator=(IrqMask const&) = delete;
    };
#endif

    // false only when the core can tell for certain that no debugger has halting debug enabled
    // (cortex_common/Debug.hpp's Core::haltingDebug() is `off`; `unknown` on the Cortex-M0+ and M4)
    inline bool haltingDebugMayBeEnabled() {
#ifdef __arm__
        return Kvasir::Core::haltingDebug() != Kvasir::Core::HaltingDebug::off;
#else
        return true;   // the host, RISC-V
#endif
    }
}   // namespace detail

/// ~0.5 s for the ack, ~0.1 s of grace, at the chip's fastest clock (see detail::minCyclesPerPoll).
template<typename... Dep>
constexpr Polls defaultPolls() {
    return {detail::pollsFor<Dep...>(500'000), detail::pollsFor<Dep...>(100'000)};
}

/// The same waits in microseconds, for a chip with a running microsecond counter.
struct Waits {
    std::uint32_t ackUs;
    std::uint32_t graceUs;
};

inline constexpr Waits defaultWaits{500'000, 100'000};

namespace detail {
    // A counter that has not moved for this long is taken as stopped (a timer whose tick is not set up).
    inline constexpr std::uint64_t stillUs = 2'000;

    // One wait: `us` by the counter while it runs, else `polls` polls. A counter that has not moved after `still`
    // polls (stillUs at the fastest clock) is taken as stopped and the polls decide.
    struct Deadline {
        std::uint32_t (*now)();
        std::uint32_t us;
        std::uint32_t polls;
        std::uint32_t still;
        std::uint32_t start{now != nullptr ? now() : 0};
        std::uint32_t done{};
        bool          moving{};

        bool expired() {
            if(now != nullptr) {
                auto const elapsed = now() - start;   // modulo 2^32: a wrap of the counter is fine
                moving             = moving || elapsed != 0;
                if(moving) { return elapsed >= us; }
                if(done >= still) { now = nullptr; }   // stopped: count from here on
            }
            return done++ >= polls;
        }
    };
}   // namespace detail

enum class Field : std::uint8_t { magic, armed, request, ack };

/// The `request` word: the sequence in bits 15:0 (never 0), the time the printer is asked to stay
/// off the chip in bits 31:16, in 100 ms units (0 = the printer's default; at most 6553.5 s).
inline constexpr std::uint32_t stayAwayUnitMs = 100;

constexpr std::uint32_t nextRequest(std::uint32_t previous,
                                    std::uint32_t stayAwayMs) {
    std::uint32_t sequence = (previous + 1U) & 0xFFFFU;
    if(sequence == 0) { sequence = 1; }
    auto const units = (stayAwayMs + stayAwayUnitMs - 1) / stayAwayUnitMs;
    return (units > 0xFFFFU ? 0xFFFFU : units) << 16 | sequence;
}

static_assert(nextRequest(0,
                          0)
                == 1
              && nextRequest(0xFFFF,
                             0)
                   == 1
              && nextRequest(5,
                             9'400)
                   == (94U << 16 | 6)
              && nextRequest(1,
                             1)
                   == (1U << 16 | 2));

/// The block's words through volatile accesses: what the firmware uses. A host test passes an
/// access of its own to announceVia() and plays the printer in it.
struct VolatileAccess {
    Block volatile& block;

    [[nodiscard]] std::uint32_t load(Field f) const {
        switch(f) {
        case Field::magic:   return block.magic;
        case Field::armed:   return block.armed;
        case Field::request: return block.request;
        case Field::ack:     return block.ack;
        }
        return 0;
    }

    void store(Field         f,
               std::uint32_t v) const {
        if(f == Field::request) { block.request = v; }   // the firmware writes nothing else
    }
};

/// The handshake. `debugEnabled` false: no debugger, return at once.
template<typename Access>
Outcome announceVia(Access const& access,
                    bool          debugEnabled,
                    Polls         polls,
                    std::uint32_t stayAwayMs = 0,
                    std::uint32_t (*nowUs)() = nullptr,
                    Waits waits              = defaultWaits) {
    if(!debugEnabled || access.load(Field::magic) != magicValue
       || access.load(Field::armed) != hostArmed)
    {
        return Outcome::notArmed;
    }
    // Interrupts masked while it waits: a busy interrupt load would stretch the grace past the host's pause and the
    // reset would fall into the host's reconnect. PRIMASK is put back for a caller that goes on.
    [[maybe_unused]] detail::IrqMask const masked{};
    std::uint32_t const request = nextRequest(access.load(Field::request), stayAwayMs);
    access.store(Field::request, request);
    std::uint32_t const still = detail::pollsFor<Access>(detail::stillUs);
    for(detail::Deadline ack{nowUs, waits.ackUs, polls.ack, still}; !ack.expired();) {
        if(access.load(Field::ack) == request) {
            // the host closes its session after the ack: let it finish before the reset
            for(detail::Deadline grace{nowUs, waits.graceUs, polls.grace, still}; !grace.expired();)
            {
                // a volatile read per pass, so the loop is not optimised away
                if(access.load(Field::magic) != magicValue) { break; }
            }
            return Outcome::acknowledged;
        }
    }
    return Outcome::timedOut;
}

template<typename... Dummy>
    requires(sizeof...(Dummy) == 0)
inline Outcome announceOn(Block volatile& block,
                          bool            debugEnabled,
                          Polls           polls,
                          std::uint32_t   stayAwayMs = 0) {
    return announceVia(VolatileAccess{block}, debugEnabled, polls, stayAwayMs);
}

inline bool printerListeningOn(Block const volatile& block) {
    return block.magic == magicValue && block.armed == hostArmed;
}

// Timed by the microsecond counter while it runs (as announce()'s waits), else counted polls: a poll of this loop
// costs far more than detail::minCyclesPerPoll (measured on an RP2040 sanitize image at 125 MHz: a 3 s count took
// ~23 s), so the polls are only the fallback.
template<typename... Dep>
inline bool waitForPrinterOn(Block const volatile& block,
                             std::uint32_t         microseconds,
                             std::uint32_t (*nowUs)() = nullptr) {
    for(detail::Deadline d{nowUs,
                           microseconds,
                           detail::pollsFor<Dep...>(microseconds),
                           detail::pollsFor<Dep...>(detail::stillUs)};
        !d.expired();)
    {
        if(printerListeningOn(block)) { return true; }
    }
    return printerListeningOn(block);
}

}   // namespace Kvasir::AnnouncedReset

// C linkage: the printer finds it in the map file by this plain name. Inline: one copy however many
// translation units include this.
extern "C" {
inline constinit Kvasir::AnnouncedReset::Block volatile kvasir_announced_reset{
  Kvasir::AnnouncedReset::magicValue,
  0,
  0,
  0};
}

namespace Kvasir::AnnouncedReset {

/// Call right before a reset the firmware does to itself (watchdog, boot ROM reboot): with a log
/// printer attached it waits until the printer is off the chip (or ~0.5 s), without one it returns
/// at once. Then reset without delay - or, for a reset that comes later (a watchdog left to run
/// out), pass how long from now the printer must stay away: the time to the reset plus a margin
/// for the boot after it. (A template only so that the chip's CpuHzCeiling is looked up late.)
template<typename... Dummy>
    requires(sizeof...(Dummy) == 0)
inline Outcome announce([[maybe_unused]] std::uint32_t stayAwayMs = 0) {
#ifdef USE_UC_LOG
    return announceVia(VolatileAccess{kvasir_announced_reset},
                       detail::haltingDebugMayBeEnabled(),
                       defaultPolls<Dummy...>(),
                       stayAwayMs,
                       &kvasir_announced_reset_time_us);
#else
    return Outcome::notArmed;
#endif
}

/// A printer has armed the block in this boot (it listens for announcements).
inline bool printerListening() {
#ifdef USE_UC_LOG
    return printerListeningOn(kvasir_announced_reset);
#else
    return false;
#endif
}

/// Waits until a printer arms the block, at most ~`microseconds` (by the chip's microsecond counter where it has
/// one; else counted polls, longer at lower clocks); true if one did. For a firmware
/// that resets itself again soon after a reset it announced: the printer is away for its pause
/// and then reconnects, and a reset that falls into that reconnect is not announced - on the
/// RP2040 that is the Rescue DP case. Call it at the start of such a boot when the boot before
/// had a printer listening (keep that in .noInit), and only then set off the next reset.
template<typename... Dummy>
    requires(sizeof...(Dummy) == 0)
inline bool waitForPrinter([[maybe_unused]] std::uint32_t microseconds) {
#ifdef USE_UC_LOG
    return waitForPrinterOn<Dummy...>(kvasir_announced_reset,
                                      microseconds,
                                      &kvasir_announced_reset_time_us);
#else
    return false;
#endif
}

}   // namespace Kvasir::AnnouncedReset
