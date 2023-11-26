#pragma once
namespace Kvasir::Nvic {
[[nodiscard]] bool primask() {
    unsigned result = 0;
    return result;
}

void disable_all() {}

[[nodiscard]] bool disable_all_and_get_old_state() {
    bool p = primask();
    disable_all();
    return !p;
}

void enable_all() {}

struct InterruptGuard {
private:
    bool oldState;

public:
    InterruptGuard() { oldState = disable_all_and_get_old_state(); }
    ~InterruptGuard() {
        if(oldState) {
            enable_all();
        }
    }
};

struct InterruptGuardAlwaysUnlock {
public:
    InterruptGuardAlwaysUnlock() { disable_all(); }
    ~InterruptGuardAlwaysUnlock() { enable_all(); }
};

}   // namespace Kvasir::Nvic
