#pragma once

#include "kvasir/Common/Interrupt.hpp"
#include "kvasir/Common/Tags.hpp"
#include "kvasir/Mpl/Algorithm.hpp"
#include "kvasir/Mpl/Utility.hpp"
#include "kvasir/Register/Register.hpp"
#include "kvasir/Util/attributes.hpp"
#include "kvasir/Util/ubsan.hpp"

#include <algorithm>
#include <cassert>

extern "C" {
[[KVASIR_RESETISR_ATTRIBUTES]] extern void ResetISR();
extern void                                _LINKER_stack_end_();
extern int                                 main();
using InitFunc = void (*)();
extern InitFunc _LINKER_init_array_start_;
extern InitFunc _LINKER_init_array_end_;

extern std::uint32_t _LINKER_data_start_flash_;
extern std::uint32_t _LINKER_data_end_flash_;
extern std::uint32_t _LINKER_data_start_;
extern std::uint32_t _LINKER_bss_end_;
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
        auto datarobegin = std::addressof(_LINKER_data_start_flash_);
        asm("" : "+l"(datarobegin)::);
        auto dataroend = std::addressof(_LINKER_data_end_flash_);
        asm("" : "+l"(dataroend)::);
        auto databegin = std::addressof(_LINKER_data_start_);
        asm("" : "+l"(databegin)::);

        while(datarobegin != dataroend) {
            dataroend = static_cast<decltype(dataroend)>(
              __builtin_assume_aligned(dataroend, alignof(decltype(*dataroend))));
            databegin = static_cast<decltype(databegin)>(
              __builtin_assume_aligned(databegin, alignof(decltype(*databegin))));
            *databegin = *datarobegin;
            ++databegin;
            ++datarobegin;
        }

        auto bssstart = databegin;
        auto bssend   = std::addressof(_LINKER_bss_end_);
        asm("" : "+l"(bssend)::);

        while(bssstart != bssend) {
            bssend = static_cast<decltype(bssend)>(
              __builtin_assume_aligned(bssend, alignof(decltype(*bssend))));
            bssstart = static_cast<decltype(bssstart)>(
              __builtin_assume_aligned(bssstart, alignof(decltype(*bssstart))));
            *bssstart = 0U;
            ++bssstart;
        }
    }

    [[gnu::always_inline]] inline void callGlobalConstructors() {
        // auto IBegin = std::addressof(_LINKER_init_array_start_);
        // asm("" : "+l"(IBegin)::);
        // auto IEnd = std::addressof(_LINKER_init_array_end_);
        // asm("" : "+l"(IEnd)::);
        // while(IBegin < IEnd) {
        //    (*IBegin)();
        //    ++IBegin;
        //}
    }

    template<typename ClockSettings, typename... Peripherals>
    struct Startup {
        [[gnu::used, gnu::section(".core_vectors")]] static constexpr Kvasir::Startup::
          NvicVectorTable<Kvasir::Startup::GetIsrPointersT<Peripherals...>> nvicIsrVectors{};

        [[noreturn,
          gnu::always_inline]] static void
        ResetISR() {
            FirstInitStep<Kvasir::Tag::User>{}();

            Kvasir::Register::apply(Kvasir::Startup::GetEarlyInitT<Peripherals...>{});

            ClockSettings::coreClockInit();

            initMemory();

            callGlobalConstructors();

            ClockSettings::peripheryClockInit();

            Kvasir::Register::apply(Kvasir::Startup::GetPowerClockInitT<Peripherals...>{});
            Kvasir::Register::apply(Kvasir::Startup::GetPinInitT<Peripherals...>{});
            Kvasir::Register::apply(Kvasir::Startup::GetPeripheryInitT<Peripherals...>{});
            Kvasir::Register::apply(Kvasir::Startup::GetInterruptInitT<Peripherals...>{});
            callPreEnableRuntimeInits<Peripherals...>();
            Kvasir::Nvic::enable_all();
            Kvasir::Register::apply(Kvasir::Startup::GetPeripheryEnableInitT<Peripherals...>{});
            callRuntimeInits<Peripherals...>();

            main();
            assert(false);
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

void log_assert(int         line,
                char const* filename,
                char const* expr);

void log_assert([[maybe_unused]] int         line,
                [[maybe_unused]] char const* filename,
                [[maybe_unused]] char const* expr) {
    UC_LOG_C("libc/libc++ assert({}) {}:{}",
             std::string_view{expr},
             std::string_view{filename},
             line);
}

}   // namespace uc_log

extern "C" {
[[gnu::used]] inline constexpr std::uint32_t __stack_chk_guard{0xdeadc0de};

[[noreturn,
  gnu::used]] inline void
__stack_chk_fail() {
    assert(false);
}
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

void operator delete(void*) {}

void operator delete(void*,
                     std::size_t) {}
