// Kvasir::Atomic::BipBuffer (Atomic/BipBuffer.hpp) against a model: random steps of writeReserve / writeReserveMax /
// writeCommit / write / readReserve / readCommit, and every sequence of 4-5 operations on sizes 2..5. After each
// step the read-out order and size() equal a std::deque; a write reservation never touches a slot holding unread
// data; once the run before a wrap is read, the space at the end is free at once (eager wrap). Index types: uint8 at
// 254, uint16 at 255 (the Size + 1 rule). Plus a two-thread run of length-prefixed frames.
#include "kvasir/Atomic/BipBuffer.hpp"
#include "test_harness.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <random>
#include <span>
#include <thread>
#include <type_traits>
#include <vector>

namespace A = Kvasir::Atomic;

static_assert(std::is_same_v<A::BipBuffer<std::uint8_t,
                                          254>::IndexType,
                             std::uint8_t>);
static_assert(std::is_same_v<A::BipBuffer<std::uint8_t,
                                          255>::IndexType,
                             std::uint16_t>);

namespace {
// One buffer and its model. Values are a running counter, so order errors show.
template<std::size_t Size>
struct Harness {
    A::BipBuffer<std::uint32_t, Size, A::SyncSignal> buf{};
    std::deque<std::uint32_t>                        model{};
    std::deque<std::uint32_t const*>                 slots{};   // where each unread element sits
    std::uint32_t                                    next{1};
    std::span<std::uint32_t>                         wres{};
    std::span<std::uint32_t const>                   rres{};
    std::uint32_t const* base{buf.writeReserveMax().data()};   // the storage
    bool                 ok{true};
    std::uint32_t        reads{};

    void fail() { ok = false; }

    // a write reservation must not overlap a slot that holds unread data
    void checkReserve(std::span<std::uint32_t> s) {
        if(s.empty()) { return; }
        auto const* const b = s.data();
        auto const* const e = s.data() + s.size();
        for(auto const* p : slots) {
            if(p >= b && p < e) { fail(); }
        }
    }

    void reserve(std::size_t n) {
        wres = buf.writeReserve(n);
        if(!wres.empty() && wres.size() != n) { fail(); }
        checkReserve(wres);
    }

    void reserveMax() {
        wres = buf.writeReserveMax();
        checkReserve(wres);
    }

    void commit(std::size_t k) {
        k = std::min(k, wres.size());
        for(std::size_t i = 0; i != k; ++i) {
            wres[i] = next;
            slots.push_back(&wres[i]);
            model.push_back(next++);
        }
        buf.writeCommit(k);
        wres = {};
    }

    void write(std::size_t n) {
        wres = {};   // write() takes a reservation of its own: the open one is gone
        std::vector<std::uint32_t> in(n);
        for(auto& v : in) { v = next++; }
        bool const wasEmpty = buf.empty();
        auto const taken    = buf.write(in);
        for(std::size_t i = 0; i != taken; ++i) {
            model.push_back(in[i]);
            std::uint32_t const* at
              = nullptr;   // placed by write(): found by its value (values are unique)
            for(std::size_t j = 0; j != Size; ++j) {
                if(base[j] == in[i]) { at = &base[j]; }
            }
            if(at == nullptr) { fail(); }
            slots.push_back(at);
        }
        next
          = static_cast<std::uint32_t>(next - (n - taken));   // the untaken ones were never written
        if(wasEmpty && n != 0 && taken == 0) { fail(); }      // an empty buffer takes something
    }

    void read(std::size_t k) {
        rres = buf.readReserve();
        if(rres.size() > model.size()) { fail(); }
        if(!model.empty() && rres.empty()) { fail(); }   // committed data must be readable
        k = std::min(k, rres.size());
        for(std::size_t i = 0; i != k; ++i) {
            if(model.empty() || rres[i] != model.front() || &rres[i] != slots.front()) {
                fail();
                return;
            }
            model.pop_front();
            slots.pop_front();
            ++reads;
        }
        buf.readCommit(k);
    }

    void checkSize() {
        if(buf.size() != model.size()) { fail(); }
    }
};

template<std::size_t Size>
bool randomRun(std::uint32_t seed,
               int           steps) {
    Harness<Size>                 h{};
    std::mt19937                  rng{seed};
    std::uniform_int_distribution op{0, 5};
    std::uniform_int_distribution len{0, static_cast<int>(Size) + 1};
    for(int i = 0; i != steps && h.ok; ++i) {
        auto const n = static_cast<std::size_t>(len(rng));
        switch(op(rng)) {
        case 0:  h.reserve(n); break;
        case 1:  h.reserveMax(); break;
        case 2:  h.commit(n); break;
        case 3:  h.write(n); break;
        default: h.read(n); break;
        }
        h.checkSize();
    }
    return h.ok;
}

// every sequence of `depth` operations, each with every length
template<std::size_t Size>
bool exhaustive(int depth) {
    constexpr int    Ops = 4;   // reserve(n)+commit(all), reserveMax+commit(n), read(n), write(n)
    int const        choices = Ops * static_cast<int>(Size + 2);
    std::vector<int> seq(static_cast<std::size_t>(depth), 0);
    while(true) {
        Harness<Size> h{};
        for(int c : seq) {
            auto const n = static_cast<std::size_t>(c / Ops);
            switch(c % Ops) {
            case 0:
                h.reserve(n);
                h.commit(n);
                break;
            case 1:
                h.reserveMax();
                h.commit(n);
                break;
            case 2:  h.read(n); break;
            default: h.write(n); break;
            }
            h.checkSize();
            if(!h.ok) { return false; }
        }
        std::size_t i = 0;
        while(i != seq.size() && ++seq[i] == choices) { seq[i++] = 0; }
        if(i == seq.size()) { return true; }
    }
}
}   // namespace

int main() {
    using Kvasir::Test::test;

    test("basics: reserve, commit, read in order; empty reserve when it does not fit");
    {
        A::BipBuffer<std::uint8_t, 8> b{};
        auto                          s = b.writeReserve(6);
        CHECK(s.size() == 6);
        for(std::size_t i = 0; i != 6; ++i) { s[i] = static_cast<std::uint8_t>(i + 1); }
        b.writeCommit(6);
        CHECK(b.size() == 6
              && b.writeReserve(3).empty());   // 2 left before the end, the reader is still at 0
        auto r = b.readReserve();
        CHECK(r.size() == 6 && r[0] == 1 && r[5] == 6);
        b.readCommit(4);
        auto w = b.writeReserve(3);   // 2 left before the end -> [0, 3), below the reader at 4
        CHECK(w.size() == 3 && w.data() == s.data());
        w[0] = 7;
        w[1] = 8;
        w[2] = 9;
        b.writeCommit(3);
        CHECK(b.size() == 5);
        r = b.readReserve();
        CHECK(r.size() == 2 && r[0] == 5 && r[1] == 6);   // the run before the wrap first
        b.readCommit(2);
        r = b.readReserve();
        CHECK(r.size() == 3 && r[0] == 7 && r[2] == 9);
        b.readCommit(3);
        CHECK(b.empty());
    }

    test("eager wrap: once the run before the wrap is read, the end is free at once");
    {
        A::BipBuffer<std::uint8_t, 8> b{};
        (void)b.writeReserve(6);
        b.writeCommit(6);
        (void)b.readReserve();
        b.readCommit(5);
        CHECK(b.writeReserve(3).size() == 3);   // 2 left before the end -> wraps: [0, 3)
        b.writeCommit(3);
        CHECK(b.readReserve().size() == 1);     // [5, 6) before the wrap
        b.readCommit(1);                        // read_ goes to 0 now, not at the next readReserve
        CHECK(b.writeReserve(5).size() == 5);   // [3, 8): w=3, r=0
    }

    test("copying write: as much as fits, in two runs");
    {
        A::BipBuffer<std::uint8_t, 8> b{};
        std::uint8_t const            in[10]{1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
        CHECK(b.write(std::span{in}.first(6)) == 6);
        (void)b.readReserve();
        b.readCommit(4);
        CHECK(b.write(std::span{in}.subspan(6)) == 4);   // [6, 8) then [0, 2): two runs
        CHECK(b.size() == 6);
    }

    test("random model runs: 400 000 steps over several sizes");
    CHECK(randomRun<2>(2, 100'000));
    CHECK(randomRun<7>(3, 100'000));
    CHECK(randomRun<16>(4, 100'000));
    CHECK(randomRun<255>(5, 100'000));

    test("exhaustive: every 4- or 5-operation sequence on sizes 2..5");
    CHECK(exhaustive<2>(5));
    CHECK(exhaustive<3>(5));
    CHECK(exhaustive<4>(4));
    CHECK(exhaustive<5>(4));

    test("two threads: length-prefixed frames, random reservations, all in order");
    {
        A::BipBuffer<std::uint8_t, 97, A::SyncThread> b{};
        constexpr std::uint32_t                       Frames = 200'000;
        bool                                          good   = true;
        // a broken buffer stalls a side: both give up after this instead of hanging the test run
        auto const        until = std::chrono::steady_clock::now() + std::chrono::seconds{20};
        std::atomic<bool> stop{false};
        std::thread       producer{[&] {
            std::mt19937 rng{7};
            for(std::uint32_t f = 0; f != Frames && !stop.load(std::memory_order_relaxed);) {
                auto const len = static_cast<std::size_t>(1 + rng() % 20);
                auto       s   = b.writeReserve(len + 1);
                if(s.empty()) { continue; }
                s[0] = static_cast<std::uint8_t>(len);
                for(std::size_t i = 0; i != len; ++i) {
                    s[1 + i] = static_cast<std::uint8_t>(f + i);
                }
                b.writeCommit(len + 1);
                ++f;
            }
        }};
        std::uint32_t     got = 0;
        while(got != Frames) {
            auto r = b.readReserve();
            if(r.empty()) {
                if(std::chrono::steady_clock::now() > until) {
                    good = false;
                    break;
                }
                continue;
            }
            // a frame never straddles the wrap: it was reserved contiguously
            auto const len = static_cast<std::size_t>(r[0]);
            if(r.size() < len + 1) {
                good = false;
                break;
            }
            for(std::size_t i = 0; i != len; ++i) {
                good = good && r[1 + i] == static_cast<std::uint8_t>(got + i);
            }
            b.readCommit(len + 1);
            ++got;
        }
        stop.store(true, std::memory_order_relaxed);
        producer.join();
        CHECK(good && got == Frames);
    }

    return Kvasir::Test::report();
}
