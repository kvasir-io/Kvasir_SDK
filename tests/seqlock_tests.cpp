#include "multicore_test.hpp"

#include <array>
#include <cstdint>
#include <kvasir/Atomic/Seqlock.hpp>
#include <thread>

namespace {
struct Probe {
    std::array<std::uint32_t, 64> words{};
};

constexpr std::uint32_t Publishes = 200'000;

void noTornReads() {
    Kvasir::Test::test("reader never sees a torn snapshot");
    Kvasir::Atomic::Seqlock<Probe> box{};

    std::uint32_t torn  = 0;
    std::uint32_t reads = 0;

    std::thread reader{[&] {
        while(true) {
            auto const p = box.read();
            ++reads;
            for(auto const w : p.words) {
                if(w != p.words[0]) {
                    ++torn;
                    break;
                }
            }
            if(p.words[0] == Publishes - 1) { break; }
        }
    }};
    for(std::uint32_t i = 0; i < Publishes; ++i) {
        Probe p{};
        p.words.fill(i);
        box.publish(p);
    }
    reader.join();
    CHECK(torn == 0);
    CHECK(reads > 0);
}

void versionAndTryRead() {
    Kvasir::Test::test("version advances by two, tryRead succeeds when quiet");
    Kvasir::Atomic::Seqlock<std::uint32_t> box{};
    CHECK(box.version() == 0);
    box.publish(7);
    CHECK(box.version() == 2);
    std::uint32_t v{};
    CHECK(box.tryRead(v));
    CHECK(v == 7);
}
}   // namespace

int main() {
    noTornReads();
    versionAndTryRead();
    return Kvasir::Test::finish();
}
