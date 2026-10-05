#pragma once
// What a fault or a panic leaves behind for the next boot, chosen once per image:
//
//     #include <kvasir/Util/CrashRecord.hpp>
//     template<>
//     inline constexpr auto Kvasir::CrashRecord::injectedPolicy<>
//       = Kvasir::CrashRecord::Full<{.stackBytes = 256}>{};
//
// Legacy (the default): Fault::lastFault (stacked frame, EXC_RETURN) and Panic::lastPanic behind a
// magic word, written only in images with uc_log.
//
// Full: CRC-checked records (Kvasir::Persistent) of the whole fault state - stacked frame, r4-r11,
// MSP/PSP/CONTROL, stack limits (Armv8-M), the fault status registers, which core, the faulting
// SP - and optionally a copy of the faulting stack, written before the fault's clean-up action
// runs. Fault::lastFaultV2, Fault::lastStack, Panic::lastPanicV2 (host tools read them by
// symbol); Boot::takePrevious() / logBoot() report them.
//
// The specialisation goes into the header with the Startup list, before the Startup is
// instantiated: one policy per image, shared by both cores' fault handlers and the panic path.
#include <cstdint>
#include <type_traits>

namespace Kvasir::CrashRecord {
struct Options {
    std::uint16_t stackBytes = 0;      // a snapshot of the faulting stack, a multiple of 4; 0: none
    bool          withoutLog = true;   // record in images without uc_log too
    bool          clearOnPowerOn = true;   // Boot::takePrevious(true) drops the records unread
};

struct Legacy {};

template<Options O = Options{}>
struct Full {
    static_assert(O.stackBytes % 4 == 0,
                  "stackBytes is whole words");
    static constexpr Options options = O;
};

template<typename...>
inline constexpr auto injectedPolicy = Legacy{};

namespace Detail {
    // A class template, not an alias: an alias with an empty pack is substituted at once and would
    // bind injectedPolicy<> where the user is defined. Here the lookup waits until PolicyOf<Anchor>
    // is instantiated with a dependent Anchor, after the application's specialisation.
    template<typename Anchor, typename... Dummy>
        requires(sizeof...(Dummy) == 0)
    struct PolicyOf {
        using type = std::remove_cvref_t<decltype(injectedPolicy<Dummy...>)>;
    };

    template<typename Anchor>
    using PolicyFor = typename PolicyOf<Anchor>::type;

    template<typename P>
    inline constexpr bool isFull = false;

    template<Options O>
    inline constexpr bool isFull<Full<O>> = true;
}   // namespace Detail
}   // namespace Kvasir::CrashRecord
