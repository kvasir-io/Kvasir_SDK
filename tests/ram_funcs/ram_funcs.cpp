// Images for test_check_ram_funcs.py: every kind of RAM function, and one broken way per CASE.
#include "kvasir/Util/attributes.hpp"

#if defined(OLD_GCC_ATTRIBUTES)
// what KVASIR_RAM_FUNC_ATTRIBUTES was before 2026-09-24: gcc's LTO puts these in .text
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
}
