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
//                                                writes armed = HostArmed
//   announce(): armed == HostArmed? else return
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
// (a watchdog timeout left to run out) asks for longer. The printer acks the whole word, so a
// printer that knows only the default acks it too.
//
// "Is a debugger there?" cannot be asked of the core on the RP2040: the Cortex-M0+ TRM (DDI0484C
// 7.2, Note) says "Software cannot access the debug registers", and Armv6-M leaves DHCSR access
// from software IMPLEMENTATION DEFINED (DDI0419E C1.6.3). So the printer says it is there by
// writing `armed`. On Armv8-M mainline (the RP2350's Cortex-M33, whose TRM 100230 C1.1.5 Table
// C1-4 lists DHCSR for Secure and Non-secure software) DHCSR.C_DEBUGEN (bit 0, DDI0553B.y
// D1.2.39) is checked as well: a probe that went away and cleared it costs no wait.
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
// allocation, no clock or timer - memory and register accesses and a counted loop only.

#include <cstdint>

namespace Kvasir::AnnouncedReset {

/// The host's side (uc_log `detail/AnnouncedReset.hpp`) has the same numbers.
inline constexpr std::uint32_t Magic     = 0x5253'5441U;   // "ATSR" in memory: the block is there
inline constexpr std::uint32_t HostArmed = 0x4D52'4148U;   // "HARM" in memory: a printer listens

struct Block {
    std::uint32_t magic;     // Magic, from the image
    std::uint32_t armed;     // HostArmed while a printer listens (written by the host)
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
    // >= 2 is assumed there. The fastest clock: RP2040 200 MHz at 1.15 V (data sheet md l.515,
    // l.8660; 133 MHz nominal), RP2350 200 MHz too - 150 MHz is its nominal clock (data sheet md
    // l.838), but firmwares run it at 200 MHz (aps-microcontroller does). So the bound is at least
    // the time asked for at the fastest clock and longer below it: the ack wait is >= 0.5 s at
    // 200 MHz (~0.75 s at 133 MHz with 6-cycle polls, ~8 s at the 12 MHz XOSC before the PLL).
    // It is spent only when a printer armed the block in this boot and does not answer.
#if defined(__ARM_ARCH_6M__)
    inline constexpr std::uint64_t MaxCoreHz        = 200'000'000;
    inline constexpr std::uint64_t MinCyclesPerPoll = 6;
#else
    inline constexpr std::uint64_t MaxCoreHz        = 200'000'000;
    inline constexpr std::uint64_t MinCyclesPerPoll = 2;
#endif
    constexpr std::uint32_t pollsFor(std::uint64_t microseconds) {
        return static_cast<std::uint32_t>(MaxCoreHz * microseconds / 1'000'000 / MinCyclesPerPoll);
    }

    // DHCSR, Armv8-M ARM DDI0553B.y D1.2.39 (0xE000EDF0, C_DEBUGEN bit 0).
    inline constexpr std::uintptr_t DhcsrAddress = 0xE000'EDF0;

    // false only when the core can tell for certain that no debugger has halting debug enabled
    inline bool haltingDebugMayBeEnabled() {
#if defined(__ARM_ARCH_8M_MAIN__)
        auto const dhcsr = *reinterpret_cast<std::uint32_t const volatile*>(DhcsrAddress);
        return (dhcsr & 1U) != 0;
#else
        return true;   // Armv6-M (Cortex-M0+: software cannot read it), RISC-V, the host
#endif
    }
}   // namespace detail

/// ~0.5 s for the ack, ~0.1 s of grace, at the fastest clock (see detail::MinCyclesPerPoll).
inline constexpr Polls DefaultPolls{detail::pollsFor(500'000), detail::pollsFor(100'000)};

enum class Field : std::uint8_t { magic, armed, request, ack };

/// The `request` word: the sequence in bits 15:0 (never 0), the time the printer is asked to stay
/// off the chip in bits 31:16, in 100 ms units (0 = the printer's default; at most 6553.5 s).
inline constexpr std::uint32_t StayAwayUnitMs = 100;

constexpr std::uint32_t nextRequest(std::uint32_t previous,
                                    std::uint32_t stayAwayMs) {
    std::uint32_t sequence = (previous + 1U) & 0xFFFFU;
    if(sequence == 0) { sequence = 1; }
    auto const units = (stayAwayMs + StayAwayUnitMs - 1) / StayAwayUnitMs;
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

    std::uint32_t load(Field f) const {
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
                    std::uint32_t stayAwayMs = 0) {
    if(!debugEnabled || access.load(Field::magic) != Magic
       || access.load(Field::armed) != HostArmed)
    {
        return Outcome::notArmed;
    }
    std::uint32_t const request = nextRequest(access.load(Field::request), stayAwayMs);
    access.store(Field::request, request);
    for(std::uint32_t i = 0; i != polls.ack; ++i) {
        if(access.load(Field::ack) == request) {
            // the host closes its session after the ack: let it finish before the reset
            for(std::uint32_t j = 0; j != polls.grace; ++j) {
                // a volatile read per pass, so the loop is not optimised away
                if(access.load(Field::magic) != Magic) { break; }
            }
            return Outcome::acknowledged;
        }
    }
    return Outcome::timedOut;
}

inline Outcome announceOn(Block volatile& block,
                          bool            debugEnabled,
                          Polls           polls,
                          std::uint32_t   stayAwayMs = 0) {
    return announceVia(VolatileAccess{block}, debugEnabled, polls, stayAwayMs);
}

inline bool printerListeningOn(Block const volatile& block) {
    return block.magic == Magic && block.armed == HostArmed;
}

inline bool waitForPrinterOn(Block const volatile& block,
                             std::uint32_t         microseconds) {
    auto const polls = detail::pollsFor(microseconds);
    for(std::uint32_t i = 0; i != polls; ++i) {
        if(printerListeningOn(block)) { return true; }
    }
    return printerListeningOn(block);
}

}   // namespace Kvasir::AnnouncedReset

// C linkage: the printer finds it in the map file by this plain name. Inline: one copy however many
// translation units include this.
extern "C" {
inline constinit Kvasir::AnnouncedReset::Block volatile kvasir_announced_reset{
  Kvasir::AnnouncedReset::Magic,
  0,
  0,
  0};
}

namespace Kvasir::AnnouncedReset {

/// Call right before a reset the firmware does to itself (watchdog, boot ROM reboot): with a log
/// printer attached it waits until the printer is off the chip (or ~0.5 s), without one it returns
/// at once. Then reset without delay - or, for a reset that comes later (a watchdog left to run
/// out), pass how long from now the printer must stay away: the time to the reset plus a margin
/// for the boot after it.
inline Outcome announce([[maybe_unused]] std::uint32_t stayAwayMs = 0) {
#if defined(USE_UC_LOG)
    return announceOn(kvasir_announced_reset,
                      detail::haltingDebugMayBeEnabled(),
                      DefaultPolls,
                      stayAwayMs);
#else
    return Outcome::notArmed;
#endif
}

/// A printer has armed the block in this boot (it listens for announcements).
inline bool printerListening() {
#if defined(USE_UC_LOG)
    return printerListeningOn(kvasir_announced_reset);
#else
    return false;
#endif
}

/// Waits until a printer arms the block, at most ~`microseconds` at the fastest clock (counted
/// polls, longer at lower clocks; see detail::MinCyclesPerPoll); true if one did. For a firmware
/// that resets itself again soon after a reset it announced: the printer is away for its pause
/// and then reconnects, and a reset that falls into that reconnect is not announced - on the
/// RP2040 that is the Rescue DP case. Call it at the start of such a boot when the boot before
/// had a printer listening (keep that in .noInit), and only then set off the next reset.
inline bool waitForPrinter([[maybe_unused]] std::uint32_t microseconds) {
#if defined(USE_UC_LOG)
    return waitForPrinterOn(kvasir_announced_reset, microseconds);
#else
    return false;
#endif
}

}   // namespace Kvasir::AnnouncedReset
