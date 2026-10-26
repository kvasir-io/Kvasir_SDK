#pragma once
// What a fault or a panic leaves behind for the next boot. Every image records both, with or without uc_log:
// CRC-checked records (Kvasir::Persistent) of the whole fault state - stacked frame, r4-r11, MSP/PSP/CONTROL, stack
// limits (Armv8-M), the fault status registers, which core, the faulting SP - and optionally a copy of the faulting
// stack, written before the fault's clean-up runs. Fault::lastFault, Fault::lastStack<N>, Panic::lastPanic (host
// tools read them by symbol); Boot::takePrevious() / logBoot() report them.
//
// The late hook RecordFormat (the table in kvasir/Port/Hooks.hpp) chooses the options, once per image, shared by both
// cores' fault handlers and the panic path; the default is Full<>:
//
//     #include <kvasir/Runtime/CrashRecord.hpp>
//     template<>
//     struct Kvasir::CrashRecord::RecordFormat<Kvasir::Tag::User> {
//         using type = Kvasir::CrashRecord::Full<{.stackBytes = 256}>;
//     };
//
// The specialisation goes into the header with the Startup list, before the Startup is instantiated.
//
// A format is a struct with its options and two parts, `Panic` and `Fault`, that carry its operations: the panic
// path, the fault handler, Boot::takePrevious()/logBoot() and BootGuard call them and never ask which format it is
// (Records<Anchor>). Full's parts are defined next to their records: Panic in Panic.hpp, Fault in FaultRecord.hpp.
// An application may inject a format of its own (the concept `Format`, here; the parts' members: `PanicPart` in
// Panic.hpp, `FaultPart` in FaultRecord.hpp):
//
//     struct MyFormat {
//         static constexpr Kvasir::CrashRecord::Options options{};   // what the fault handler records
//         struct Panic {                                   // where a panic goes and how it comes back
//             static bool recorded();                      //   a record is there (not taking it)
//             static std::optional<std::uint32_t> cause(); //   its Panic::Cause, not taking it
//             static void record(Kvasir::Panic::Info const&);   // from the panic handler: masked, no log
//             static std::optional<Kvasir::Panic::FullRecord> take();   // once: what the boot report prints
//         };
//         using Fault = Kvasir::CrashRecord::Full<>::Fault;   // or a part of its own
//     };
//     template<> struct Kvasir::CrashRecord::RecordFormat<Kvasir::Tag::User> { using type = MyFormat; };
//
// The fault handler writes Fault::lastFault and lastStack with the format's `options`, so a Fault part of its own
// reads those. tests/crash_record_format_tests.cpp is one.
#include "kvasir/Common/Tags.hpp"
#include "kvasir/Port/Hooks.hpp"

#include <concepts>
#include <cstdint>
#include <type_traits>

namespace Kvasir::CrashRecord {
struct Options {
    std::uint16_t stackBytes = 0;   // a snapshot of the faulting stack, a multiple of 4; 0: none
    bool          clearOnPowerOn = true;   // Boot::takePrevious(true) drops the records unread
};

// The CRC-checked records (Panic::lastPanic, Fault::lastFault, Fault::lastStack). The parts: Panic.hpp,
// FaultRecord.hpp.
template<Options O = Options{}>
struct Full {
    static_assert(O.stackBytes % 4 == 0,
                  "stackBytes is whole words");
    static constexpr Options options = O;
    struct Panic;
    struct Fault;
};

// What makes a type a format: its options, a Panic part and a Fault part (the parts' members: PanicPart, FaultPart,
// checked where the parts are used - the parts may be complete only later, next to their records).
template<typename P>
concept Format = requires {
    { P::options } -> std::convertible_to<Options>;
    typename P::Panic;
    typename P::Fault;
};

// The image's format (header comment), a late hook: type, Full<Options> or the application's own.
template<typename T, typename... Ts>
struct RecordFormat {
    using type = Full<>;
};

namespace Detail {
    // A class template, not an alias: an alias with an empty pack is substituted at once and would bind the hook
    // where the user is defined. Here the lookup (Port::LateT on Anchor) waits until PolicyOf<Anchor> is instantiated
    // with a dependent Anchor, after the application's specialisation.
    template<typename Anchor, typename... Dummy>
        requires(sizeof...(Dummy) == 0)
    struct PolicyOf {
        using type = typename Port::LateT<RecordFormat<Tag::User>, Anchor, Dummy...>::type;
        static_assert(Format<type>,
                      "Kvasir::CrashRecord::RecordFormat<Tag::User>::type is not a format: it "
                      "needs its options, a Panic and "
                      "a Fault part (CrashRecord.hpp)");
    };
}   // namespace Detail

// The image's format, looked up where Anchor (or the pack) is dependent - after the application's specialisation.
// Every parameter goes on to the class template, so the alias stays dependent.
template<typename Anchor, typename... Dummy>
using Records = typename Detail::PolicyOf<Anchor, Dummy...>::type;
}   // namespace Kvasir::CrashRecord
