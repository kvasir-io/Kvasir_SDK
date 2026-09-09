#include "multicore_test.hpp"

#include <cstdint>
#include <kvasir/Atomic/Queue.hpp>
#include <thread>

namespace {
constexpr std::uint32_t Items = 500'000;

void inOrderAcrossThreads() {
    Kvasir::Test::test("SPSC queue delivers every item in order across threads");
    static_assert(std::is_same_v<Kvasir::Atomic::DefaultSync, Kvasir::Atomic::SyncThread>,
                  "the multicore tests build with KVASIR_MULTICORE");
    Kvasir::Atomic::Queue<std::uint32_t, 64, Kvasir::Atomic::OverFlowPolicyIgnore> q{};

    std::uint32_t errors = 0;
    std::thread   consumer{[&] {
        for(std::uint32_t i = 0; i < Items; ++i) {
            std::uint32_t v{};
            while(!q.pop_into(v)) {}
            if(v != i) { ++errors; }
        }
    }};
    for(std::uint32_t i = 0; i < Items; ++i) {
        while(q.size() == q.max_size()) {}
        q.push(i);
    }
    consumer.join();
    CHECK(errors == 0);
    CHECK(q.empty());
}

void capacity() {
    Kvasir::Test::test("capacity is Size - 1 and overflow is ignored by policy");
    Kvasir::Atomic::Queue<int, 4, Kvasir::Atomic::OverFlowPolicyIgnore> q{};
    CHECK(q.max_size() == 3);
    q.push(1);
    q.push(2);
    q.push(3);
    q.push(4);   // dropped
    CHECK(q.size() == 3);
    int v{};
    CHECK(q.pop_into(v) && v == 1);
    CHECK(q.pop_into(v) && v == 2);
    CHECK(q.pop_into(v) && v == 3);
    CHECK(!q.pop_into(v));
}
}   // namespace

int main() {
    inOrderAcrossThreads();
    capacity();
    return Kvasir::Test::finish();
}
