#pragma once
#include "uc_log/uc_log.hpp"

#include <cassert>
#include <cstdint>

namespace Kvasir { namespace Fault {
    struct CleanUpActionNone {
        void operator()() {}
    };

    struct FaultActionAssert {
        [[noreturn]] void operator()() {
            while(true) {
                asm("bkpt 6" : : :);
            }
        }
    };

    template<typename TCleanUpAction = CleanUpActionNone, typename TFaultAction = FaultActionAssert>
    struct Handler {
        static void CleanUp() { TCleanUpAction{}(); }

        static void Fault() {
            [[maybe_unused]] auto const faultInfo = Core::Fault::info();
            UC_LOG_C("{}: fault flags: {:#010x}", faultInfo.first, faultInfo.second);
            TFaultAction{}();
        }

        [[gnu::naked]] static void onIsr() {
            asm("mov sp, %0 \n" : : "l"(std::addressof(_LINKER_stack_end_)) :);
            asm("blx %0 \n" : : "l"(std::addressof(CleanUp)) :);
            asm("blx %0 \n" : : "l"(std::addressof(Fault)) :);
        }

        static constexpr auto initStepPeripheryEnable = MPL::list(
          Nvic::makeEnable(Nvic::InterruptOffsetTraits<>::FaultInterruptIndexsNeedEnable{}));

        template<typename... Ts>
        static constexpr auto makeIsr(brigand::list<Ts...>) {
            return brigand::list<
              Kvasir::Nvic::Isr<std::addressof(onIsr), Nvic::Index<Ts::value>>...>{};
        }

        using Isr = decltype(makeIsr(
          typename Kvasir::Nvic::InterruptOffsetTraits<>::FaultInterruptIndexs{}));
    };
}}   // namespace Kvasir::Fault
