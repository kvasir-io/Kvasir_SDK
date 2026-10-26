#pragma once
// A sentinel at the very bottom of core 0's stack (.stackProtector, placed first in the stack section), checked once
// per main-loop turn. An overwritten sentinel - the stack grew into its last bytes - is a panic of its own cause,
// Panic::Cause::stackOverflow: no program site (pc 0: the check finds the damage a turn later, wherever the loop is),
// detail = the sentinel words overwritten. Not an assert: it does not depend on NDEBUG.

#include "kvasir/Port/Extends.hpp"
#include "kvasir/Runtime/Panic.hpp"
#include "kvasir/Util/attributes.hpp"

#ifdef USE_UC_LOG
    #include "uc_log/uc_log.hpp"
#endif

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace Kvasir {

template<std::size_t Size = 32>
struct StackProtector {
    static constexpr std::uint32_t sentinelVal{0x55AA55AA};

    static_assert(Size % sizeof(sentinelVal) == 0,
                  "size not valid");

    using Sentinel_t = std::array<std::decay_t<decltype(sentinelVal)>, Size / sizeof(sentinelVal)>;

    [[KVASIR_SECTION_MEMBER(".stackProtector")]] static inline Sentinel_t sentinel{};

    [[gnu::always_inline]] static void runtimeInit() {
        std::fill(sentinel.begin(), sentinel.end(), sentinelVal);
    }

    // The main loop's turn (the verbs of Port/Hooks.hpp): inline and cheap while the sentinel holds, and "nothing to
    // wait for" for a sleeping loop (Startup::runTurn): the check runs whenever the loop turns anyway. Listed (Extends
    // below) or called by hand, not both.
    static Turn turn() {
        if(std::any_of(sentinel.begin(), sentinel.end(), [](auto v) { return v != sentinelVal; })) {
            overflowed();
        }
        return Turn::idle();
    }

    using Extends = Startup::Extend<Hook::MainLoop, &StackProtector::turn>;

private:
    // A broken sentinel. Cold and out of line, so each inlined turn() is a compare loop and a call; pc 0 makes
    // that safe (a helper that took its caller's site would name itself).
    [[noreturn,
      gnu::cold,
      gnu::noinline]] static void
    overflowed() {
        auto const broken = static_cast<std::uint32_t>(
          std::count_if(sentinel.begin(), sentinel.end(), [](auto v) { return v != sentinelVal; }));
#ifdef USE_UC_LOG
        UC_LOG_SCOPE_MODULE("assert");   // the module __stack_chk_fail and assert log under
        KVASIR_DETAIL_FATAL_AT(
          Panic::Cause::stackOverflow,
          0,
          broken,
          "stack overflow: {} of {} StackProtector sentinel words at the stack bottom overwritten",
          broken,
          sentinel.size());
#else
        KVASIR_DETAIL_FATAL_AT(Panic::Cause::stackOverflow, 0, broken);
#endif
    }
};
}   // namespace Kvasir
