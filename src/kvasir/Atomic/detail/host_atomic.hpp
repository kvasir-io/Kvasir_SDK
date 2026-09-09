#pragma once

namespace Kvasir::Nvic {
[[nodiscard]] inline bool primask() {
    unsigned result = 0;
    return result;
}

inline void disable_all() {}

[[nodiscard]] inline bool disable_all_and_get_old_state() {
    bool p = primask();
    disable_all();
    return !p;
}

inline void enable_all() {}

struct InterruptGuard {
private:
    bool oldState;

public:
    InterruptGuard() { oldState = disable_all_and_get_old_state(); }

    ~InterruptGuard() {
        if(oldState) { enable_all(); }
    }
};

struct InterruptGuardAlwaysUnlock {
public:
    InterruptGuardAlwaysUnlock() { disable_all(); }

    ~InterruptGuardAlwaysUnlock() { enable_all(); }
};

}   // namespace Kvasir::Nvic

namespace Kvasir::Atomic {
inline void onSecondaryCoreReset() {}
}   // namespace Kvasir::Atomic
