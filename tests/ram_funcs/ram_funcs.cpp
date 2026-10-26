// Images for test_check_ram_funcs.py: every kind of RAM function, and one broken way per CASE.
#if CASE == 6 || CASE == 7
    #define KVASIR_CORE_SCRATCH 1   // KVASIR_CORE1_CODE: a function in the scratch bank (test.ld)
#endif
#include "kvasir/Util/attributes.hpp"

#if defined(OLD_GCC_ATTRIBUTES)
// what KVASIR_RAM_FUNC_ATTRIBUTES used to be: gcc's LTO puts these in .text
    #undef KVASIR_RAM_FUNC_ATTRIBUTES
    #define KVASIR_RAM_FUNC_ATTRIBUTES gnu::section(".data#"), gnu::noinline, gnu::long_call
#endif

int volatile sink;

[[gnu::noinline]] void inFlash(int v) { sink = v; }

struct Member {
    [[KVASIR_RAM_FUNC_ATTRIBUTES]] static void run(int v) {
        KVASIR_RAM_FUNC_MARK();
        sink = v;
    }
};

template<int N>
[[KVASIR_RAM_FUNC_ATTRIBUTES]] void templated(int v) {
    KVASIR_RAM_FUNC_MARK();
    sink = v + N;
}

[[KVASIR_RAM_FUNC_ATTRIBUTES]] inline void inlined(int v) {
    KVASIR_RAM_FUNC_MARK();
    sink = v * 3;
}

[[KVASIR_RAM_FUNC_ATTRIBUTES]] void helper(int v) {
    KVASIR_RAM_FUNC_MARK();
#if CASE == 3
    inFlash(v);   // reached from caller(): the chain has to be reported
#else
    sink = v - 1;
#endif
}

[[KVASIR_RAM_FUNC_ATTRIBUTES]] void caller(int v) {
    KVASIR_RAM_FUNC_MARK();
#if CASE == 2
    inFlash(v);   // a RAM function calling flash directly
#endif
    helper(v);
}

[[KVASIR_RAM_FUNC_ATTRIBUTES]] void callsFlashOnPurpose(int v) {
    KVASIR_RAM_FUNC_MARK_CALLS_FLASH();
    inFlash(v);
}

[[KVASIR_RAM_FUNC_ATTRIBUTES]] void neverCalled(int v) {   // garbage-collected where it can be
    KVASIR_RAM_FUNC_MARK();
    sink = v;
}

#if CASE == 5
// a RAM function reading a constant table in flash (.rodata)
[[KVASIR_RAM_FUNC_ATTRIBUTES]] void readsFlashTable(int v) {
    KVASIR_RAM_FUNC_MARK();
    static constexpr unsigned char table[16]{3, 1, 4, 1, 5, 9, 2, 6, 5, 3, 5, 8, 9, 7, 9, 3};
    sink = table[v & 15];
}
#endif

#if CASE == 6 || CASE == 7
// a scratch-bank function (RAM outside `ram`) reached from a RAM function: followed like RAM
[[KVASIR_CORE1_CODE]] void inScratch(int v) {
    #if CASE == 6
    inFlash(v);   // reached through it: reported
    #else
    sink = v + 7;
    #endif
}

[[KVASIR_RAM_FUNC_ATTRIBUTES]] void callsScratch(int v) {
    KVASIR_RAM_FUNC_MARK();
    inScratch(v);
}
#endif

#if CASE == 8 || CASE == 9
    #include "kvasir/Register/AtomicFactories.hpp"
    #include "kvasir/Register/Register.hpp"
    #include "kvasir/Register/Views.hpp"

    #include <array>
    #include <cstdint>

// two registers of a made-up peripheral, as svd_converter writes them
struct RegA {
    using Addr = Kvasir::Register::Address<0x40010000, 0xFFFF0000, 0x00000000, std::uint32_t>;
    static constexpr Kvasir::Register::FieldLocation<Addr,
                                                     Kvasir::Register::maskFromRange(15, 8),
                                                     Kvasir::Register::ReadWriteAccess,
                                                     std::uint32_t>
      hi{};
    static constexpr Kvasir::Register::FieldLocation<Addr,
                                                     Kvasir::Register::maskFromRange(7, 0),
                                                     Kvasir::Register::ReadWriteAccess,
                                                     std::uint32_t>
                                   lo{};
    static constexpr std::uint32_t resetValue = 0x00001200;
    using fields = Kvasir::Register::Fields<decltype(hi), decltype(lo)>;

    template<typename... Actions>
    [[gnu::always_inline]] static constexpr auto overrideDefaults(Actions const&... actions) {
        return Kvasir::Register::overrideDefaults<Kvasir::Register::DefaultValues<RegA>>(
          actions...);
    }
};

struct RegB {
    using Addr = Kvasir::Register::Address<0x40010004, 0xFFFFFFFE, 0x00000000, std::uint32_t>;
    static constexpr Kvasir::Register::FieldLocation<Addr,
                                                     Kvasir::Register::maskFromRange(0, 0),
                                                     Kvasir::Register::ReadWriteAccess,
                                                     bool>
      en{};
};
#endif

#if CASE == 8
// the field API in a RAM function (FrequencyCounter::measureRaw reads FC0 this way): nothing it reaches may stay a
// call - with inlining off, only always_inline holds, as -Oz with UBSan once left FieldTuple's std::array
// operator[] out of line in flash
[[KVASIR_RAM_FUNC_ATTRIBUTES]] void registerAccess(int v) {
    KVASIR_RAM_FUNC_MARK();
    using namespace Kvasir::Register;
    auto const both        = apply(read(RegA::hi, RegA::lo));   // one register, two fields: get<I>
    sink                   = int(get<0>(both) + get<1>(both));
    std::uint32_t const lo = apply(read(RegA::lo));   // one field: the conversion
    sink                   = int(lo);
    auto const two         = apply(read(RegA::lo, RegB::en));   // two registers: the index of each
    sink                   = int(get<0>(two)) + (get<1>(two) ? 1 : 0);
    apply(write(RegA::lo, std::uint32_t(v)), set(RegB::en));
    apply(clear(RegB::en));
    apply(RegA::overrideDefaults(
      write(RegA::lo, std::uint32_t(v))));                 // the bundle: hi from its reset value
    bool const b3 = apply(read(bit<3, bool>(RegA::lo)));   // one bit of a field, as a bool
    sink          = b3 ? 1 : 0;
    apply(write(fieldAt<9>(RegA{}), value<std::uint32_t, 3>()));   // the field holding bit 9 (hi)
    apply(atomic(set(bit<1>(RegA::hi))));                          // an atomic set of one bit
}
#endif

#if CASE == 9
// the control: std::array's operator[] in a RAM function - the build mode of CASE 8 has to leave it a call to flash,
// or CASE 8 proves nothing
[[KVASIR_RAM_FUNC_ATTRIBUTES]] void stdArrayIndex(int v) {
    KVASIR_RAM_FUNC_MARK();
    std::array<unsigned, 1> a{unsigned(v)};
    sink = int(a[0]);
}
#endif

#if CASE == 10
    #include "kvasir/Register/Memory.hpp"

    #include <cstdint>

// Kvasir::Memory in a RAM function (a flash or OTP word read with XIP off): as CASE 8, nothing may stay a call
[[KVASIR_RAM_FUNC_ATTRIBUTES]] void memoryAccess(int v) {
    KVASIR_RAM_FUNC_MARK();
    std::uint32_t words[3]{};
    Kvasir::Memory::loadWords(0x10000000U, words, 3);
    sink = int(words[0] + words[2] + Kvasir::Memory::load<std::uint8_t>(0x10U));
    Kvasir::Memory::store<std::uint16_t>(0x20000000U, std::uint16_t(v));
}
#endif

#if CASE == 11
    #include "Barrier.hpp"         // core_cortex_common (the test passes its directory)
    #include "InterruptMask.hpp"   // Core::IrqGuard, IrqState

    #include <cstdint>

// The PRIMASK API and the barriers in a RAM function (XIP-off code masks with them): as CASE 8, nothing may stay a call
[[KVASIR_RAM_FUNC_ATTRIBUTES]] void irqAccess(int v) {
    KVASIR_RAM_FUNC_MARK();
    {
        Kvasir::Core::IrqGuard const guard{};
        sink = v + int(guard.wasMasked()) + int(Kvasir::Core::irqsMasked());
        Kvasir::Core::dsb();
        Kvasir::Core::isb();
    }
    auto const state = Kvasir::Core::IrqState::saveAndMask();
    Kvasir::Core::dmb();
    sink = sink + int(state.wasMasked());
    state.restore();
    Kvasir::Core::maskIrqs();
    Kvasir::Core::unmaskIrqs();
}
#endif

#if CASE == 12
    #include "Delay.hpp"   // core_cortex_common (the test passes its directory)

    #include <cstdint>

// The core's counted waits in a RAM function (XipControl's chip-select dwells run with XIP off): as CASE 8, nothing may
// stay a call
[[KVASIR_RAM_FUNC_ATTRIBUTES]] void delayAccess(int v) {
    KVASIR_RAM_FUNC_MARK();
    Kvasir::Core::spinCycles<64>();
    Kvasir::Core::spinCycles(static_cast<std::uint32_t>(v));
    sink = v;
}
#endif

#if CASE == 4
    #include "unmarked.hpp"   // a file of its own: the source check reads text, not #if
#endif

extern "C" [[gnu::section(".text.main")]] void entry() {
    Member::run(sink);
    templated<2>(sink);
    templated<5>(sink);
    inlined(sink);
    caller(sink);
    callsFlashOnPurpose(sink);
#if CASE == 4
    unmarked(sink);
#endif
#if CASE == 5
    readsFlashTable(sink);
#endif
#if CASE == 6 || CASE == 7
    callsScratch(sink);
#endif
#if CASE == 8
    registerAccess(sink);
#endif
#if CASE == 9
    stdArrayIndex(sink);
#endif
#if CASE == 10
    memoryAccess(sink);
#endif
#if CASE == 11
    irqAccess(sink);
#endif
#if CASE == 12
    delayAccess(sink);
#endif
}
