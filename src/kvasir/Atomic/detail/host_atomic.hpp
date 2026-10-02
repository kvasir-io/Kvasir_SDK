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

struct Global {};

// The target's shape (arm_Common_atomic.hpp), so code written against it compiles on the host;
// there is no interrupt to mask here.
template<typename T>
struct InterruptGuard {
private:
    bool oldState;

public:
    InterruptGuard() { oldState = disable_all_and_get_old_state(); }

    InterruptGuard(InterruptGuard const&)            = delete;
    InterruptGuard& operator=(InterruptGuard const&) = delete;

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
