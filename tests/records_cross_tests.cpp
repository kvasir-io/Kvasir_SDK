// The records a firmware leaves for a debugger against their one host decoder, uc_log's printer
// (printer/TargetRecords.hpp), which kvasir_bench asks over the control socket. uc_log cannot include the SDK's
// headers - it is its own project, and the SDK includes it, not the other way round - so this test holds the
// printer's copy to them: every Persistent record's id, size and field offsets, the name tables (Panic::Cause,
// FullRecord::Flag, BootGuard::Failure and Flag) and the stack paint; the sanitizer counter in records_cross_ubsan.cpp.
// A drift fails the compile. Trace::Ring has its own test (trace_cross_tests.cpp: its descriptor holds a pointer, so
// it is decoded by offsets, not by its host struct).
#include "kvasir/Health/HealthRecords.hpp"
#include "kvasir/Runtime/BootGuardRules.hpp"
#include "kvasir/Runtime/FaultRecord.hpp"
#include "kvasir/Runtime/Panic.hpp"
#include "kvasir/Runtime/StackUsage.hpp"
#include "uc_log/printer/TargetRecords.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace {
namespace records = uc_log::printer::records;

// ---- Panic::Cause ------------------------------------------------------------------------------
constexpr bool causeNamesMatch() {
    for(std::size_t i = 0; i != records::panicCauses.size(); ++i) {
        if(records::panicCauses[i]
           != std::string_view{Kvasir::Panic::name(static_cast<Kvasir::Panic::Cause>(i))})
        {
            return false;
        }
    }
    return true;
}

static_assert(records::panicCauses.size() == Kvasir::Panic::causeCount,
              "uc_log's PanicCauses misses a Cause, or has one too many");
static_assert(causeNamesMatch(),
              "uc_log's PanicCauses is not Panic.hpp's name(), in Cause order");

// ---- Fault::lastFault ---------------------------------------------------------------------------
using Kvasir::Fault::FullRecord;
static_assert(records::faultId == Kvasir::Fault::RecordTag::id);
static_assert(sizeof(records::Fault) == sizeof(FullRecord));
static_assert(records::faultSize == sizeof(Kvasir::Fault::lastFault));
static_assert(records::Fault::coreShift == FullRecord::coreShift
              && records::Fault::archShift == FullRecord::archShift);

// a field's word in the SDK's record, and the word its name has in uc_log's table
constexpr std::size_t nameAt(std::string_view name) {
    for(std::size_t i = 0; i != records::faultFieldNames.size(); ++i) {
        if(records::faultFieldNames[i] == name) { return i; }
    }
    return ~std::size_t{};
}

#define KVASIR_FAULT_FIELD(sdk, uc, name)                                               \
    static_assert(offsetof(FullRecord, sdk) == (uc), "uc_log's Fault: " #sdk " moved"); \
    static_assert(nameAt(name) * 4 == offsetof(FullRecord, sdk),                        \
                  "FaultFieldNames: \"" name "\" is not " #sdk "'s word")

KVASIR_FAULT_FIELD(count,
                   offsetof(records::Fault,
                            count),
                   "count");
KVASIR_FAULT_FIELD(flags,
                   offsetof(records::Fault,
                            flags),
                   "flags");
KVASIR_FAULT_FIELD(r0,
                   offsetof(records::Fault,
                            r)
                     + 0,
                   "r0");
KVASIR_FAULT_FIELD(r1,
                   offsetof(records::Fault,
                            r)
                     + 4,
                   "r1");
KVASIR_FAULT_FIELD(r2,
                   offsetof(records::Fault,
                            r)
                     + 8,
                   "r2");
KVASIR_FAULT_FIELD(r3,
                   offsetof(records::Fault,
                            r)
                     + 12,
                   "r3");
KVASIR_FAULT_FIELD(r12,
                   offsetof(records::Fault,
                            r12),
                   "r12");
KVASIR_FAULT_FIELD(lr,
                   offsetof(records::Fault,
                            lr),
                   "lr");
KVASIR_FAULT_FIELD(pc,
                   offsetof(records::Fault,
                            pc),
                   "pc");
KVASIR_FAULT_FIELD(xpsr,
                   offsetof(records::Fault,
                            xpsr),
                   "xpsr");
KVASIR_FAULT_FIELD(r4,
                   offsetof(records::Fault,
                            high)
                     + 0,
                   "r4");
KVASIR_FAULT_FIELD(r5,
                   offsetof(records::Fault,
                            high)
                     + 4,
                   "r5");
KVASIR_FAULT_FIELD(r6,
                   offsetof(records::Fault,
                            high)
                     + 8,
                   "r6");
KVASIR_FAULT_FIELD(r7,
                   offsetof(records::Fault,
                            high)
                     + 12,
                   "r7");
KVASIR_FAULT_FIELD(r8,
                   offsetof(records::Fault,
                            high)
                     + 16,
                   "r8");
KVASIR_FAULT_FIELD(r9,
                   offsetof(records::Fault,
                            high)
                     + 20,
                   "r9");
KVASIR_FAULT_FIELD(r10,
                   offsetof(records::Fault,
                            high)
                     + 24,
                   "r10");
KVASIR_FAULT_FIELD(r11,
                   offsetof(records::Fault,
                            high)
                     + 28,
                   "r11");
KVASIR_FAULT_FIELD(excReturn,
                   offsetof(records::Fault,
                            excReturn),
                   "exc_return");
KVASIR_FAULT_FIELD(msp,
                   offsetof(records::Fault,
                            msp),
                   "msp");
KVASIR_FAULT_FIELD(psp,
                   offsetof(records::Fault,
                            psp),
                   "psp");
KVASIR_FAULT_FIELD(control,
                   offsetof(records::Fault,
                            control),
                   "control");
KVASIR_FAULT_FIELD(sp,
                   offsetof(records::Fault,
                            sp),
                   "sp");
KVASIR_FAULT_FIELD(cfsr,
                   offsetof(records::Fault,
                            cfsr),
                   "cfsr");
KVASIR_FAULT_FIELD(hfsr,
                   offsetof(records::Fault,
                            hfsr),
                   "hfsr");
KVASIR_FAULT_FIELD(mmfar,
                   offsetof(records::Fault,
                            mmfar),
                   "mmfar");
KVASIR_FAULT_FIELD(bfar,
                   offsetof(records::Fault,
                            bfar),
                   "bfar");
KVASIR_FAULT_FIELD(shcsr,
                   offsetof(records::Fault,
                            shcsr),
                   "shcsr");
KVASIR_FAULT_FIELD(icsr,
                   offsetof(records::Fault,
                            icsr),
                   "icsr");
KVASIR_FAULT_FIELD(sfsr,
                   offsetof(records::Fault,
                            sfsr),
                   "sfsr");
KVASIR_FAULT_FIELD(sfar,
                   offsetof(records::Fault,
                            sfar),
                   "sfar");
KVASIR_FAULT_FIELD(msplim,
                   offsetof(records::Fault,
                            msplim),
                   "msplim");
KVASIR_FAULT_FIELD(psplim,
                   offsetof(records::Fault,
                            psplim),
                   "psplim");
#undef KVASIR_FAULT_FIELD
static_assert(records::faultFieldNames.size() * 4 == sizeof(FullRecord),
              "a name for every word");

// FullRecord::Flag by bit
constexpr std::uint32_t sdkFaultFlags[]{FullRecord::frameValid,
                                        FullRecord::calleeSaved,
                                        FullRecord::faultRegs,
                                        FullRecord::fpFrame,
                                        FullRecord::stackLimits,
                                        FullRecord::secureRegs,
                                        FullRecord::stackCopied};
static_assert(std::size(sdkFaultFlags) == records::faultFlagNames.size(),
              "a name for every Flag");

constexpr bool faultFlagsMatch() {
    constexpr std::uint32_t uc[]{records::Fault::frameValid,
                                 records::Fault::calleeSaved,
                                 records::Fault::faultRegs,
                                 records::Fault::fpFrame,
                                 records::Fault::stackLimits,
                                 records::Fault::secureRegs,
                                 records::Fault::stackCopied};
    for(std::size_t i = 0; i != std::size(sdkFaultFlags); ++i) {
        if(sdkFaultFlags[i] != (1U << i) || uc[i] != sdkFaultFlags[i]) { return false; }
    }
    return true;
}

static_assert(faultFlagsMatch(),
              "FullRecord::Flag is bit i for FaultFlagNames[i], and uc_log's Flag the same");
static_assert(records::faultFlagNames[0] == "frameValid"
              && records::faultFlagNames[6] == "stackCopied");

// ---- Fault::lastStack<N> ------------------------------------------------------------------------
static_assert(records::stackSnapshotId == Kvasir::Fault::StackTag::id);
using Snapshot = Kvasir::Fault::StackSnapshot<64>;
static_assert(offsetof(Snapshot,
                       from)
                == 0
              && offsetof(Snapshot,
                          bytes)
                   == 4);
static_assert(offsetof(Snapshot,
                       words)
              == std::size_t{4} * records::stackSnapshotHeadWords);
static_assert(records::stackSnapshotSize(64) == sizeof(Kvasir::Fault::lastStack<64>),
              "the size rule from N");
static_assert(records::stackSnapshotSize(256) == sizeof(Kvasir::Fault::lastStack<256>));
static_assert(records::stackSnapshotBytes("Kvasir::Fault::lastStack<(unsigned short)256>") == 256U);
static_assert(records::stackSnapshotBytes("Kvasir::Fault::lastStack<64u>") == 64U);

// ---- Panic::lastPanic ---------------------------------------------------------------------------
static_assert(records::panicId == Kvasir::Panic::RecordTag::id);
static_assert(sizeof(records::Panic) == sizeof(Kvasir::Panic::FullRecord));
static_assert(records::panicSize == sizeof(Kvasir::Panic::lastPanic));
// a field moved or swapped (timeLow/timeHigh) fails here, not only a changed size
static_assert(offsetof(records::Panic,
                       count)
              == offsetof(Kvasir::Panic::FullRecord,
                          count));
static_assert(offsetof(records::Panic,
                       cause)
              == offsetof(Kvasir::Panic::FullRecord,
                          cause));
static_assert(offsetof(records::Panic,
                       pc)
              == offsetof(Kvasir::Panic::FullRecord,
                          pc));
static_assert(offsetof(records::Panic,
                       detail)
              == offsetof(Kvasir::Panic::FullRecord,
                          detail));
static_assert(offsetof(records::Panic,
                       core)
              == offsetof(Kvasir::Panic::FullRecord,
                          core));
static_assert(offsetof(records::Panic,
                       sp)
              == offsetof(Kvasir::Panic::FullRecord,
                          sp));
static_assert(offsetof(records::Panic,
                       timeLow)
              == offsetof(Kvasir::Panic::FullRecord,
                          timeLow));
static_assert(offsetof(records::Panic,
                       timeHigh)
              == offsetof(Kvasir::Panic::FullRecord,
                          timeHigh));
static_assert(records::Panic{.timeLow  = 5,
                             .timeHigh = 1}
                .timeUs()
              == [] {
                     Kvasir::Panic::FullRecord r{};
                     r.timeLow  = 5;
                     r.timeHigh = 1;
                     return r.timeUs();
                 }());

// ---- Health::lastStarvation, lastRecovery ---------------------------------------------------------
using Kvasir::Health::Recoveries;
using Kvasir::Health::Starvation;
static_assert(records::starvationId == Kvasir::Health::StarvationTag::id);
static_assert(sizeof(records::Starvation) == sizeof(Starvation));
static_assert(records::recordSize<records::Starvation> == sizeof(Kvasir::Health::lastStarvation));
static_assert(offsetof(records::Starvation,
                       check)
              == offsetof(Starvation,
                          check));
static_assert(offsetof(records::Starvation,
                       nameHash)
              == offsetof(Starvation,
                          nameHash));
static_assert(offsetof(records::Starvation,
                       silentMs)
              == offsetof(Starvation,
                          silentMs));
static_assert(offsetof(records::Starvation,
                       allowedMs)
              == offsetof(Starvation,
                          allowedMs));
static_assert(offsetof(records::Starvation,
                       uptimeMs)
              == offsetof(Starvation,
                          uptimeMs));
static_assert(offsetof(records::Starvation,
                       silentMask)
              == offsetof(Starvation,
                          silentMask));

static_assert(records::recoveriesId == Kvasir::Health::RecoveriesTag::id);
static_assert(sizeof(records::Recoveries) == sizeof(Recoveries));
static_assert(records::recordSize<records::Recoveries> == sizeof(Kvasir::Health::lastRecovery));
static_assert(offsetof(records::Recoveries,
                       total)
              == offsetof(Recoveries,
                          total));
static_assert(offsetof(records::Recoveries,
                       lastCheck)
              == offsetof(Recoveries,
                          lastCheck));
static_assert(offsetof(records::Recoveries,
                       lastNameHash)
              == offsetof(Recoveries,
                          lastNameHash));
static_assert(offsetof(records::Recoveries,
                       lastUptimeMs)
              == offsetof(Recoveries,
                          lastUptimeMs));
static_assert(offsetof(records::Recoveries,
                       inProgress)
              == offsetof(Recoveries,
                          inProgress));
static_assert(offsetof(records::Recoveries,
                       escalatedAttempts)
              == offsetof(Recoveries,
                          escalatedAttempts));
// the inProgress encoding: the SDK's functions, uc_log's members
constexpr auto inProgress = Kvasir::Health::recoveryInProgress(5, 3);
static_assert(records::Recoveries{.inProgress = inProgress}.inProgressCheck()
                == Kvasir::Health::inProgressCheck(inProgress)
              && records::Recoveries{.inProgress = inProgress}.inProgressAttempt()
                   == Kvasir::Health::inProgressAttempt(inProgress));

// ---- BootGuard::state -----------------------------------------------------------------------------
using Kvasir::BootGuard::State;
static_assert(records::guardId == Kvasir::BootGuard::Tag::id);
static_assert(sizeof(records::GuardState) == sizeof(State));
static_assert(records::recordSize<records::GuardState> == sizeof(Kvasir::BootGuard::state));
static_assert(offsetof(records::GuardState,
                       bootCount)
              == offsetof(State,
                          bootCount));
static_assert(offsetof(records::GuardState,
                       consecutive)
              == offsetof(State,
                          consecutive));
static_assert(offsetof(records::GuardState,
                       total)
              == offsetof(State,
                          total));
static_assert(offsetof(records::GuardState,
                       lastFailure)
              == offsetof(State,
                          lastFailure));
static_assert(offsetof(records::GuardState,
                       lastDetail)
              == offsetof(State,
                          lastDetail));
static_assert(offsetof(records::GuardState,
                       flags)
              == offsetof(State,
                          flags));
static_assert(offsetof(records::GuardState,
                       reserved)
              == offsetof(State,
                          reserved));

// Failure by value: a value per name (the enum has no name table of its own)
using Kvasir::BootGuard::Failure;

struct FailureName {
    Failure          value;
    std::string_view name;
};

constexpr FailureName failures[]{
  {      Failure::none,       "none"},
  {     Failure::panic,      "panic"},
  {     Failure::fault,      "fault"},
  {  Failure::watchdog,   "watchdog"},
  {Failure::starvation, "starvation"},
  {  Failure::external,   "external"}
};

constexpr bool failuresMatch() {
    if(std::size(failures) != records::guardFailures.size()) { return false; }
    for(auto const& f : failures) {
        auto const at = static_cast<std::size_t>(f.value);
        if(at >= records::guardFailures.size() || records::guardFailures[at] != f.name) {
            return false;
        }
    }
    return true;
}

static_assert(failuresMatch(),
              "uc_log's GuardFailures is not BootGuard::Failure by value");
// external is the last name; a Failure appended after it has no row above (the enum has no count) - BootGuardRules'
// own switch over Failure fails -Wswitch then, and a row goes here with its name in guardFailures
static_assert(static_cast<std::size_t>(Failure::external) + 1 == records::guardFailures.size());

// Flag by bit
namespace Guard = Kvasir::BootGuard;
static_assert(Guard::runOpen == 1U << 0 && records::guardFlagNames[0] == "runOpen");
static_assert(Guard::plannedResetPending == 1U << 1
              && records::guardFlagNames[1] == "plannedResetPending");
static_assert(Guard::halted == 1U << 2 && records::guardFlagNames[2] == "halted");
static_assert(Guard::safeMode == 1U << 3 && records::guardFlagNames[3] == "safeMode");
static_assert(Guard::healthyThisRun == 1U << 4 && records::guardFlagNames[4] == "healthyThisRun");
static_assert(records::guardFlagNames.size() == 5,
              "a Flag added: name it here and in GuardFlagNames");

// ---- StackUsage ----------------------------------------------------------------------------------
static_assert(records::stackPattern == Kvasir::StackUsage::pattern);
}   // namespace

int main() { return 0; }
