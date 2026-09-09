#include "multicore_test.hpp"

#include <cstdint>
#include <kvasir/Atomic/Spinlock.hpp>
#include <thread>

namespace {
constexpr std::uint32_t Iterations = 200'000;

template<typename Lock>
void exactCount(char const* name) {
    Kvasir::Test::test(name);
    Lock          lock{};
    std::uint32_t counter = 0;

    auto worker = [&] {
        for(std::uint32_t i = 0; i < Iterations; ++i) {
            Kvasir::Atomic::LockGuard const guard{lock};
            ++counter;
        }
    };
    std::thread a{worker};
    std::thread b{worker};
    a.join();
    b.join();
    CHECK(counter == 2 * Iterations);
}

void tryLock() {
    Kvasir::Test::test("try_lock");
    Kvasir::Atomic::Spinlock lock{};
    CHECK(lock.try_lock());
    CHECK(!lock.try_lock());
    lock.unlock();
    CHECK(lock.try_lock());
    lock.unlock();
}
}   // namespace

int main() {
    exactCount<Kvasir::Atomic::Spinlock>("spinlock keeps every increment");
    exactCount<Kvasir::Atomic::CriticalSection>("critical section keeps every increment");
    tryLock();
    return Kvasir::Test::finish();
}
