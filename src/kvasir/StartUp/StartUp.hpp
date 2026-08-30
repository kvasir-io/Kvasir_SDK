#pragma once

#include "kvasir/Common/Interrupt.hpp"
#include "kvasir/Common/Tags.hpp"
#include "kvasir/Mpl/Algorithm.hpp"
#include "kvasir/Mpl/Utility.hpp"
#include "kvasir/Register/Register.hpp"
#include "kvasir/StartUp/IsrProfiler.hpp"
#include "kvasir/StartUp/LinkerSymbols.hpp"
#include "kvasir/Util/attributes.hpp"
#include "kvasir/Util/ubsan.hpp"
#include "uc_log/uc_log.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>

extern "C" {
[[KVASIR_RESETISR_ATTRIBUTES]] extern void ResetISR();
extern int                                 main();
}

namespace Kvasir { namespace Startup {
    namespace Detail {
        using namespace MPL;
        namespace br = brigand;

        template<typename T>
        struct Listify {
            static_assert(AlwaysFalse<T>::value,
                          "implausible type");
        };

        template<typename T, typename U>
        struct Listify<Register::Action<T, U>> : br::list<Register::Action<T, U>> {};

        template<typename... Ts>
        struct Listify<br::list<Ts...>> : br::list<Ts...> {};

        template<typename T, typename = void>
        struct GetEarlyInit : br::list<> {};

        template<typename T>
        struct GetEarlyInit<T, VoidT<decltype(T::earlyInit)>>
          : Listify<RemoveCVT<decltype(T::earlyInit)>> {};

        template<typename T, typename = void>
        struct GetPowerClockInit : br::list<> {};

        template<typename T>
        struct GetPowerClockInit<T, VoidT<decltype(T::powerClockEnable)>>
          : Listify<RemoveCVT<decltype(T::powerClockEnable)>> {};

        template<typename T, typename = void, typename = void>
        struct GetPinInit : br::list<> {};

        template<typename T>
        struct GetPinInit<T, void, VoidT<decltype(T::initStepPinConfig)>>
          : Listify<RemoveCVT<decltype(T::initStepPinConfig)>> {};

        template<typename T, typename = void, typename = void>
        struct GetPeripheryInit : br::list<> {};

        template<typename T>
        struct GetPeripheryInit<T, void, VoidT<decltype(T::initStepPeripheryConfig)>>
          : Listify<RemoveCVT<decltype(T::initStepPeripheryConfig)>> {};

        template<typename T, typename = void, typename = void>
        struct GetInterruptInit : br::list<> {};

        template<typename T>
        struct GetInterruptInit<T, void, VoidT<decltype(T::initStepInterruptConfig)>>
          : Listify<RemoveCVT<decltype(T::initStepInterruptConfig)>> {};

        template<typename T, typename = void, typename = void>
        struct GetPeripheryEnableInit : br::list<> {};

        template<typename T>
        struct GetPeripheryEnableInit<T, void, VoidT<decltype(T::initStepPeripheryEnable)>>
          : Listify<RemoveCVT<decltype(T::initStepPeripheryEnable)>> {};

        template<int I>
        struct IsIsrByIndex {
            template<typename T>
            struct Apply : Bool<(T::IType::value == I)>::type {};
        };

        template<int I, typename TList, typename TModList>
        struct CompileIsrPointerList;

        template<int I, typename... Ts, typename TModList>
        struct CompileIsrPointerList<I, br::list<Ts...>, TModList>
          : CompileIsrPointerList<
              I + 1,
              br::list<Ts...,
                       GetT<TModList, Template<IsIsrByIndex<I>::template Apply>, Nvic::UnusedIsr>>,
              TModList> {};

        template<typename... Ts, typename TModList>
        struct CompileIsrPointerList<Nvic::InterruptOffsetTraits<void>::end,
                                     br::list<Ts...>,
                                     TModList> : br::list<Ts...> {};

        // predicate returning result of left < right for RegisterOptions
        template<typename TLeft, typename TRight>
        struct ListLengthLess : Bool<(SizeT<TLeft>::value < SizeT<TRight>::value)> {};

        using ListLengthLessP = Template<ListLengthLess>;

        template<typename TOut, typename TList>
        struct Merge;

        template<typename... Os, typename... Ts>
        struct Merge<br::list<Os...>, br::list<br::list<>, Ts...>>
          : Merge<   // if next is empty list remove it and continue
              br::list<Os...>,
              br::list<Ts...>> {};

        template<typename... Os, typename... Ts>
        struct Merge<br::list<Os...>, br::list<Ts...>>
          : Merge<br::list<Os..., br::flatten<br::list<AtT<Ts, Int<0>>...>>>,
                  br::list<RemoveT<Ts, Int<0>, Int<1>>...>> {};

        template<typename... Os>
        struct Merge<br::list<Os...>, br::list<>> : br::list<Os...> {};

        template<typename T, typename = void, typename = void>
        struct ExtractIsr : br::list<> {};

        template<typename T, typename U>
        struct ExtractIsr<T, U, VoidT<typename T::Isr>> : T::Isr {};

        template<typename T>
        struct ExtractIsr<T, void, VoidT<decltype(T::isr)>>
          : std::remove_const_t<decltype(T::isr)> {};

    }   // namespace Detail

    template<typename... Ts>
    struct GetIsrPointers
      : Detail::CompileIsrPointerList<
          Nvic::InterruptOffsetTraits<void>::begin,
          brigand::list<Nvic::Isr<std::addressof(_LINKER_stack_end_), Nvic::Index<0>>,
                        Nvic::Isr<ResetISR, Nvic::Index<0>>>,
          brigand::flatten<brigand::list<typename Detail::ExtractIsr<Ts>::type...>>> {};

    template<typename... Ts>
    using GetIsrPointersT = typename GetIsrPointers<Ts...>::type;

    template<typename... Ts>
    struct GetEarlyInit {
        // make list of lists of actions corresponding to each sequence for each module
        using FlattenedSequencePieces
          = brigand::list<brigand::flatten<typename Detail::GetEarlyInit<Ts>::type>...>;
        using type = brigand::flatten<FlattenedSequencePieces>;
    };

    template<typename... Ts>
    using GetEarlyInitT = typename GetEarlyInit<Ts...>::type;

    template<typename... Ts>
    struct GetPowerClockInit {
        // make list of lists of actions corresponding to each sequence for each module
        using FlattenedSequencePieces
          = brigand::list<brigand::flatten<typename Detail::GetPowerClockInit<Ts>::type>...>;
        using type = brigand::flatten<FlattenedSequencePieces>;
    };

    template<typename... Ts>
    using GetPowerClockInitT = typename GetPowerClockInit<Ts...>::type;

    template<typename... Ts>
    struct GetPinInit {
        // make list of lists of actions corresponding to each sequence for each module
        using FlattenedSequencePieces
          = brigand::list<brigand::flatten<typename Detail::GetPinInit<Ts>::type>...>;
        using type = brigand::flatten<FlattenedSequencePieces>;
    };

    template<typename... Ts>
    using GetPinInitT = typename GetPinInit<Ts...>::type;

    template<typename... Ts>
    struct GetPeripheryInit {
        // make list of lists of actions corresponding to each sequence for each module
        using FlattenedSequencePieces
          = brigand::list<brigand::flatten<typename Detail::GetPeripheryInit<Ts>::type>...>;
        using type = brigand::flatten<FlattenedSequencePieces>;
    };

    template<typename... Ts>
    using GetPeripheryInitT = typename GetPeripheryInit<Ts...>::type;

    template<typename... Ts>
    struct GetInterruptInit {
        // make list of lists of actions corresponding to each sequence for each module
        using FlattenedSequencePieces
          = brigand::list<brigand::flatten<typename Detail::GetInterruptInit<Ts>::type>...>;
        using type = brigand::flatten<FlattenedSequencePieces>;
    };

    template<typename... Ts>
    using GetInterruptInitT = typename GetInterruptInit<Ts...>::type;

    template<typename... Ts>
    struct GetPeripheryEnableInit {
        // make list of lists of actions corresponding to each sequence for each module
        using FlattenedSequencePieces
          = brigand::list<brigand::flatten<typename Detail::GetPeripheryEnableInit<Ts>::type>...>;
        using type = brigand::flatten<FlattenedSequencePieces>;
    };

    template<typename... Ts>
    using GetPeripheryEnableInitT = typename GetPeripheryEnableInit<Ts...>::type;

    template<typename T>
    struct NvicVectorTable;

    template<typename... Ts>
    struct NvicVectorTable<brigand::list<Ts...>> {
        std::array<Kvasir::Nvic::IsrFunctionPointer, sizeof...(Ts)> data{Ts::value...};
    };

    template<typename T>
    struct has_runtimeInit {
        template<typename U>
        static constexpr std::false_type test(...) noexcept {
            return {};
        }

        template<typename U>
        static constexpr auto test(U*) noexcept ->
          typename std::is_same<void,
                                decltype(U::runtimeInit())>::type {
            return {};
        }

        static constexpr bool value = test<T>(nullptr);
    };

    template<typename T>
    struct has_preEnableRuntimeInit {
        template<typename U>
        static constexpr std::false_type test(...) noexcept {
            return {};
        }

        template<typename U>
        static constexpr auto test(U*) noexcept ->
          typename std::is_same<void,
                                decltype(U::preEnableRuntimeInit())>::type {
            return {};
        }

        static constexpr bool value = test<T>(nullptr);
    };

    template<typename T>
    void callPreEnableRuntimeInit() {
        if constexpr(has_preEnableRuntimeInit<T>::value) { T::preEnableRuntimeInit(); }
    }

    template<typename... Ts>
    void callPreEnableRuntimeInits() {
        (callPreEnableRuntimeInit<Ts>(), ...);
    }

    template<typename T>
    void callRuntimeInit() {
        if constexpr(has_runtimeInit<T>::value) { T::runtimeInit(); }
    }

    template<typename... Ts>
    void callRuntimeInits() {
        (callRuntimeInit<Ts>(), ...);
    }

    [[gnu::always_inline]] inline void initMemory() {
        auto data_start = std::addressof(_LINKER_data_start_);
        asm("" : "+l"(data_start)::);

        auto data_start_flash = std::addressof(_LINKER_data_start_flash_);
        asm("" : "+l"(data_start_flash)::);

        auto data_size = reinterpret_cast<std::size_t>(std::addressof(_LINKER_data_size_));
        asm("" : "+l"(data_size)::);

        std::memcpy(data_start, data_start_flash, data_size);

        auto bss_start = std::addressof(_LINKER_bss_start_);
        asm("" : "+l"(bss_start)::);

        auto bss_size = reinterpret_cast<std::size_t>(std::addressof(_LINKER_bss_size_));
        asm("" : "+l"(bss_size)::);

        std::memset(bss_start, 0, bss_size);
    }

    [[gnu::always_inline]] inline void callGlobalConstructors() {
        auto init_begin = std::addressof(_LINKER_init_array_start_);
        asm("" : "+l"(init_begin)::);

        auto init_end = std::addressof(_LINKER_init_array_end_);
        asm("" : "+l"(init_end)::);

        while(init_begin < init_end) {
            (*init_begin)();
            ++init_begin;
        }
    }

    namespace Detail {

        struct NoOpStartupHook {
            [[gnu::always_inline]] void operator()() const noexcept {}
        };

        // Shared ResetISR body. Hook is called immediately after FirstInitStep,
        // before any ISR fires — used by StartupWithProfiling to enable the
        // DWT cycle counter; NoOpStartupHook for plain Startup.
        template<typename ClockSettings, typename... Peripherals>
        struct StartupImpl {
            template<typename Hook = NoOpStartupHook>
            [[noreturn,
              gnu::always_inline]] static void
            ResetISR() {
                FirstInitStep<Kvasir::Tag::User>{}();
                Hook{}();

                Kvasir::Register::apply(GetEarlyInitT<Peripherals...>{});

                ClockSettings::coreClockInit();

                initMemory();

                callGlobalConstructors();

                ClockSettings::peripheryClockInit();

                Kvasir::Register::apply(GetPowerClockInitT<Peripherals...>{});
                Kvasir::Register::apply(GetPinInitT<Peripherals...>{});
                Kvasir::Register::apply(GetPeripheryInitT<Peripherals...>{});
                Kvasir::Register::apply(GetInterruptInitT<Peripherals...>{});
                callPreEnableRuntimeInits<Peripherals...>();
                Kvasir::Nvic::enable_all();
                Kvasir::Register::apply(GetPeripheryEnableInitT<Peripherals...>{});
                callRuntimeInits<Peripherals...>();

                main();
                assert(false);
            }
        };

    }   // namespace Detail

    // ISR pointer builder with profiling transformation applied to the
    // peripheral ISR list before it reaches CompileIsrPointerList.
    // Stack-end and ResetISR seed entries bypass transformation.
    template<typename Policy, typename TimeSource, typename... Ts>
    struct GetIsrPointersWithProfiling
      : Detail::CompileIsrPointerList<
          Nvic::InterruptOffsetTraits<void>::begin,
          brigand::list<Nvic::Isr<std::addressof(_LINKER_stack_end_), Nvic::Index<0>>,
                        Nvic::Isr<ResetISR, Nvic::Index<0>>>,
          typename TransformIsrList<
            Policy,
            TimeSource,
            brigand::flatten<brigand::list<typename Detail::ExtractIsr<Ts>::type...>>>::type> {};

    template<typename Policy, typename TimeSource, typename... Ts>
    using GetIsrPointersWithProfilingT =
      typename GetIsrPointersWithProfiling<Policy, TimeSource, Ts...>::type;

    template<typename ClockSettings, typename... Peripherals>
    struct Startup {
        [[gnu::used, gnu::section(".core_vectors")]] static constexpr Kvasir::Startup::
          NvicVectorTable<Kvasir::Startup::GetIsrPointersT<Peripherals...>> nvicIsrVectors{};

        [[noreturn,
          gnu::always_inline]] static void
        ResetISR() {
            Detail::StartupImpl<ClockSettings, Peripherals...>::ResetISR();
        }
    };

    template<typename TimeSource>
    struct EnableTimeSourceHook {
        [[gnu::always_inline]] void operator()() const noexcept { TimeSource::enable(); }
    };

    // Primary template — only specialised for Startup<> below.
    template<typename BaseStartup,
             typename ProfilePolicy = ProfileNonePolicy,
             typename TimeSource    = DwtTimeSource>
    struct StartupWithProfiling;

    // Partial specialisation: unwraps Startup<ClockSettings, Peripherals...>
    // so callers only need to pass their existing Startup alias plus the two
    // profiling-specific arguments (policy and time source).
    template<typename ClockSettings,
             typename... Peripherals,
             typename ProfilePolicy,
             typename TimeSource>
    struct StartupWithProfiling<Startup<ClockSettings, Peripherals...>, ProfilePolicy, TimeSource> {
        [[gnu::used, gnu::section(".core_vectors")]] static constexpr Kvasir::Startup::
          NvicVectorTable<GetIsrPointersWithProfilingT<ProfilePolicy, TimeSource, Peripherals...>>
            nvicIsrVectors{};

        [[noreturn,
          gnu::always_inline]] static void
        ResetISR() {
            Detail::StartupImpl<ClockSettings, Peripherals...>::template ResetISR<
              EnableTimeSourceHook<TimeSource>>();
        }

        // The full compiled ISR list (wrappers + plain Isr entries)
        using IsrList = GetIsrPointersWithProfilingT<ProfilePolicy, TimeSource, Peripherals...>;

        // Only the IsrProfileWrapper entries
        using ProfiledWrapperList = FilterWrappersT<IsrList>;

        static constexpr std::size_t profiledCount = brigand::size<ProfiledWrapperList>::value;

        // Returns a fixed-size array of snapshots, one per profiled ISR.
        // Size is known at compile time — no heap allocation.
        static std::array<IsrProfileSnapshot,
                          profiledCount>
        getProfiles() noexcept {
            return getProfilesImpl(ProfiledWrapperList{});
        }

        static void printProfiles() {
            UC_LOG_T("{:#^32}", " ISR profiles "_sc);
            for([[maybe_unused]] auto const& p : getProfiles()) {
                UC_LOG_T("  isr[{:3}]  calls: {}", p.isrIndex, p.callCount);
                UC_LOG_T("    interval  min:{:>10}  avg:{:>10}  max:{:>10}  cyc",
                         p.minIntervalCycles,
                         p.avgIntervalCycles,
                         p.maxIntervalCycles);
                UC_LOG_T("    duration  min:{:>10}  avg:{:>10}  max:{:>10}  cyc",
                         p.minDurationCycles,
                         p.avgDurationCycles,
                         p.maxDurationCycles);
            }
        }

    private:
        template<typename... Wrappers>
        static std::array<IsrProfileSnapshot,
                          sizeof...(Wrappers)>
        getProfilesImpl(brigand::list<Wrappers...>) noexcept {
            return {IsrProfileStats<Wrappers::IType::value, TimeSource>::snapshot()...};
        }
    };

}}   // namespace Kvasir::Startup

#ifdef __arm__

namespace uc_log {
template<int Line,
         typename Filename,
         typename Expr>
inline void log_assert() {
    UC_LOG_IMPL(uc_log::LogLevel::crit,
                Line,
                std::string_view{Filename{}()},
                sc::escape(
                  sc::create([]() { return std::string_view{Expr{}()}; }),
                  [](auto c) { return c == '{' || c == '}'; },
                  [](auto c) { return c; }));
}

    // llvm-libc's and libc++'s patched headers already declare log_assert (-Wredundant-decls);
    // without any prototype clang's -Wmissing-prototypes objects instead
    #if !defined(LIBC_NAMESPACE) && !defined(_LIBCPP_VERSION)
void log_assert(int         line,
                char const* filename,
                char const* expr);
    #endif

void log_assert([[maybe_unused]] int         line,
                [[maybe_unused]] char const* filename,
                [[maybe_unused]] char const* expr) {
    UC_LOG_C("libc/libc++ assert({}) {}:{}",
             std::string_view{expr},
             std::string_view{filename},
             line);
}

}   // namespace uc_log

    // newlib's assert() pulls stdio/_write/_sbrk and libstdc++'s __throw_* pull abort -> malloc;
    // strong definitions here keep those archive members out. gnu::noreturn, not [[noreturn]]:
    // the standard attribute must be on newlib's first declaration.
    #if defined(__NEWLIB__)
extern "C" {
[[gnu::noreturn,
  gnu::used]] void
__assert_func(char const* file,
              int         line,
              char const* /*func*/,
              char const* expr) {
    uc_log::log_assert(line, file, expr);
    while(true) { asm volatile("bkpt 5" : : :); }
}

[[gnu::noreturn,
  gnu::used]] void
abort() {
    UC_LOG_C("abort() called (libstdc++ __throw_* or libc)");
    while(true) { asm volatile("bkpt 5" : : :); }
}

void _exit(int);   // newlib declares it in <unistd.h>, which is not included here

[[gnu::noreturn,
  gnu::used]] void
_exit(int) {
    abort();
}
}
    #endif
    #if !defined(__clang__)
// gcc has no [[clang::no_destroy]]; accept and ignore the atexit() registration of static
// destructors - firmware never exits
extern "C" {
[[gnu::used]] int atexit(void (*)()) { return 0; }
}
    #endif
    #if defined(__GLIBCXX__)
// libstdc++'s _GLIBCXX_ASSERTIONS reporter (the sanitize variant turns those on)
namespace std {
[[gnu::noreturn,
  gnu::used]] void
__glibcxx_assert_fail(char const* file,
                      int         line,
                      char const* /*func*/,
                      char const* cond) noexcept {
    uc_log::log_assert(line, file, cond);
    while(true) { asm volatile("bkpt 5" : : :); }
}
}   // namespace std
    #endif

    #if defined(__clang__) && defined(KVASIR_COMPILER_RT_LIBGCC) && defined(LIBC_NAMESPACE)
// clang lowers struct copies and memset to the AEABI helpers, which neither libgcc nor llvm-libc
// provides. Argument order matters: __aeabi_memset takes (dest, n, value), the reverse of memset.
extern "C" {
[[gnu::used]] inline void __aeabi_memcpy(void*       d,
                                         void const* s,
                                         std::size_t n) {
    std::memcpy(d, s, n);
}

[[gnu::used]] inline void __aeabi_memcpy4(void*       d,
                                          void const* s,
                                          std::size_t n) {
    std::memcpy(d, s, n);
}

[[gnu::used]] inline void __aeabi_memcpy8(void*       d,
                                          void const* s,
                                          std::size_t n) {
    std::memcpy(d, s, n);
}

[[gnu::used]] inline void __aeabi_memmove(void*       d,
                                          void const* s,
                                          std::size_t n) {
    std::memmove(d, s, n);
}

[[gnu::used]] inline void __aeabi_memmove4(void*       d,
                                           void const* s,
                                           std::size_t n) {
    std::memmove(d, s, n);
}

[[gnu::used]] inline void __aeabi_memmove8(void*       d,
                                           void const* s,
                                           std::size_t n) {
    std::memmove(d, s, n);
}

[[gnu::used]] inline void __aeabi_memset(void*       d,
                                         std::size_t n,
                                         int         v) {
    std::memset(d, v, n);
}

[[gnu::used]] inline void __aeabi_memset4(void*       d,
                                          std::size_t n,
                                          int         v) {
    std::memset(d, v, n);
}

[[gnu::used]] inline void __aeabi_memset8(void*       d,
                                          std::size_t n,
                                          int         v) {
    std::memset(d, v, n);
}

[[gnu::used]] inline void __aeabi_memclr(void*       d,
                                         std::size_t n) {
    std::memset(d, 0, n);
}

[[gnu::used]] inline void __aeabi_memclr4(void*       d,
                                          std::size_t n) {
    std::memset(d, 0, n);
}

[[gnu::used]] inline void __aeabi_memclr8(void*       d,
                                          std::size_t n) {
    std::memset(d, 0, n);
}
}
    #endif

extern "C" {
// EHABI personality routines. Nothing here unwinds, but the sanitizers make clang emit .ARM.exidx
// entries whose personality reference would pull libgcc's whole unwinder out of the archive;
// defining the routines keeps it out.
[[gnu::used]] inline void __aeabi_unwind_cpp_pr0() {}

[[gnu::used]] inline void __aeabi_unwind_cpp_pr1() {}

[[gnu::used]] inline void __aeabi_unwind_cpp_pr2() {}

[[gnu::used]] inline constexpr std::uint32_t __stack_chk_guard{0xdeadc0de};

[[noreturn,
  gnu::used]] inline void
__stack_chk_fail() {
    assert(false);
}
}

[[noreturn]] inline void Kvasir::Nvic::DefaultIsrs::onIsr() {
    UC_LOG_C("unhandled interrupt fired, IRQ={}", []() {
        std::uint32_t ipsr_val{};
        asm volatile("mrs %0, ipsr" : "=r"(ipsr_val));
        return static_cast<std::int32_t>(ipsr_val) - 16;
    }());
    while(true) { asm volatile("bkpt 7" : : :); }
}

    #define KVASIR_START(Startup)                        \
        [[KVASIR_RESETISR_ATTRIBUTES]] void ResetISR() { \
            (void)Startup::nvicIsrVectors.data[1];       \
            Startup::ResetISR();                         \
        }
#else
    #define KVASIR_START(Startup)   // TODO
#endif

extern "C" {
[[noreturn,
  gnu::used]] inline int
__aeabi_idiv0(int);

[[noreturn,
  gnu::used]] inline int
__aeabi_idiv0(int) {
    assert(false);
}

[[noreturn,
  gnu::used]] inline long long
__aeabi_ldiv0(long long);

[[noreturn,
  gnu::used]] inline long long
__aeabi_ldiv0(long long) {
    assert(false);
}
}

namespace std {
//void terminate() noexcept { assert(false); }
}   // namespace std

void operator delete(void*) noexcept {}

void operator delete(void*,
                     std::size_t) noexcept {}
