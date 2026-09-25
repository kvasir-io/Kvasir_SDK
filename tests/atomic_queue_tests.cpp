// Tests for Kvasir::Atomic::Queue, the single producer / single consumer ring buffer.
//
// The queue keeps one slot free to tell "full" from "empty", so a Queue<T, Size> holds at
// most Size - 1 elements. The tests cover the index arithmetic around that wrap point, the
// bulk range overloads, the overflow policy hook, and finally an interleaved
// producer/consumer run over a queue small enough that the indices wrap many times.
#include "kvasir/Atomic/Queue.hpp"
#include "test_harness.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <type_traits>
#include <vector>

using Kvasir::Atomic::OverFlowPolicyIgnore;
using Kvasir::Atomic::Queue;

namespace {

// an overflow policy that records how often it fired instead of asserting
int overflowCount = 0;

struct CountingOverflowPolicy {
    void operator()() { ++overflowCount; }
};

template<typename T, std::size_t Size>
using TestQueue = Queue<T, Size, CountingOverflowPolicy>;

void resetOverflow() { overflowCount = 0; }

}   // namespace

// ---------------------------------------------------------------------------
// the index type is the smallest one that can address Size slots
static_assert(std::is_same_v<Queue<int,
                                   4>::IndexType,
                             std::uint8_t>);
static_assert(std::is_same_v<Queue<int,
                                   254>::IndexType,
                             std::uint8_t>);
static_assert(std::is_same_v<Queue<int,
                                   256>::IndexType,
                             std::uint16_t>);
static_assert(std::is_same_v<Queue<int,
                                   65534>::IndexType,
                             std::uint16_t>);
static_assert(std::is_same_v<Queue<int,
                                   65536>::IndexType,
                             std::uint32_t>);

// note: Size 255 and Size 65535 are rejected by the static_assert inside Queue, because the
// index type has to be able to represent a value strictly greater than Size

// ---------------------------------------------------------------------------
static void emptyQueue() {
    Kvasir::Test::test("emptyQueue");
    resetOverflow();

    TestQueue<int, 4> q;
    CHECK(q.empty());
    CHECK_EQ(q.size(), 0U);
    CHECK_EQ(q.max_size(), 3U);   // one slot is reserved to distinguish full from empty

    int out = 42;
    CHECK(!q.pop_into(out));
    CHECK_EQ(out, 42);   // a failed pop leaves the target alone

    // popping an empty queue is a no-op rather than an error
    q.pop();
    CHECK(q.empty());
    CHECK_EQ(overflowCount, 0);

    // front() on an empty queue reports through the overflow policy
    (void)q.front();
    CHECK_EQ(overflowCount, 1);
}

static void pushPopSingle() {
    Kvasir::Test::test("pushPopSingle");
    resetOverflow();

    TestQueue<int, 4> q;

    q.push(1);
    CHECK(!q.empty());
    CHECK_EQ(q.size(), 1U);
    CHECK_EQ(q.front(), 1);

    q.push(2);
    q.push(3);
    CHECK_EQ(q.size(), 3U);
    CHECK_EQ(q.size(), q.max_size());
    CHECK_EQ(q.front(), 1);   // front is still the oldest element

    // FIFO order
    int out = 0;
    CHECK(q.pop_into(out));
    CHECK_EQ(out, 1);
    CHECK(q.pop_into(out));
    CHECK_EQ(out, 2);
    CHECK_EQ(q.size(), 1U);
    CHECK(q.pop_into(out));
    CHECK_EQ(out, 3);
    CHECK(q.empty());
    CHECK(!q.pop_into(out));

    CHECK_EQ(overflowCount, 0);
}

static void pushOnFullQueue() {
    Kvasir::Test::test("pushOnFullQueue");
    resetOverflow();

    TestQueue<int, 4> q;
    q.push(1);
    q.push(2);
    q.push(3);
    CHECK_EQ(q.size(), 3U);
    CHECK_EQ(overflowCount, 0);

    // the fourth push does not fit: Size 4 holds 3 elements
    q.push(4);
    CHECK_EQ(overflowCount, 1);
    CHECK_EQ(q.size(), 3U);   // the queue is unchanged

    // and the contents are untouched
    int out = 0;
    CHECK(q.pop_into(out));
    CHECK_EQ(out, 1);

    // after making room a push succeeds again
    resetOverflow();
    q.push(5);
    CHECK_EQ(overflowCount, 0);
    CHECK_EQ(q.size(), 3U);
}

static void popDiscard() {
    Kvasir::Test::test("popDiscard");
    resetOverflow();

    TestQueue<int, 8> q;
    q.push(1);
    q.push(2);
    q.push(3);

    q.pop();
    CHECK_EQ(q.size(), 2U);
    CHECK_EQ(q.front(), 2);

    q.pop();
    q.pop();
    CHECK(q.empty());

    // popping past the end does nothing
    q.pop();
    CHECK(q.empty());
    CHECK_EQ(q.size(), 0U);
}

static void clearResetsTheQueue() {
    Kvasir::Test::test("clearResetsTheQueue");
    resetOverflow();

    TestQueue<int, 8> q;
    q.push(1);
    q.push(2);
    CHECK_EQ(q.size(), 2U);

    q.clear();
    CHECK(q.empty());
    CHECK_EQ(q.size(), 0U);

    // the queue is fully usable afterwards
    q.push(7);
    CHECK_EQ(q.size(), 1U);
    CHECK_EQ(q.front(), 7);
}

// pushing and popping repeatedly must keep working as the head and tail indices wrap around
// the end of the buffer
static void indexWrapAround() {
    Kvasir::Test::test("indexWrapAround");
    resetOverflow();

    TestQueue<int, 4> q;

    // many more iterations than the buffer has slots, so the indices wrap repeatedly
    for(int i = 0; i != 100; ++i) {
        q.push(i);
        CHECK_EQ(q.size(), 1U);

        int out = -1;
        CHECK(q.pop_into(out));
        CHECK_EQ(out, i);
        CHECK(q.empty());
    }
    CHECK_EQ(overflowCount, 0);

    // keep the queue partially filled while wrapping, so head and tail are never equal
    q.push(0);
    q.push(1);
    for(int i = 2; i != 100; ++i) {
        q.push(i);
        CHECK_EQ(q.size(), 3U);

        int out = -1;
        CHECK(q.pop_into(out));
        CHECK_EQ(out, i - 2);
        CHECK_EQ(q.size(), 2U);
    }
    CHECK_EQ(overflowCount, 0);
}

// filling to capacity, draining and refilling, at every possible starting offset
static void fillDrainAtEveryOffset() {
    Kvasir::Test::test("fillDrainAtEveryOffset");
    resetOverflow();

    TestQueue<int, 5> q;   // holds 4 elements

    for(int offset = 0; offset != 5; ++offset) {
        // move the start index along by pushing and popping `offset` times
        for(int i = 0; i != offset; ++i) {
            q.push(-1);
            int discard = 0;
            CHECK(q.pop_into(discard));
        }
        CHECK(q.empty());

        // fill to capacity
        for(int i = 0; i != 4; ++i) { q.push(i); }
        CHECK_EQ(q.size(), 4U);
        CHECK_EQ(q.size(), q.max_size());

        // one more must overflow
        resetOverflow();
        q.push(99);
        CHECK_EQ(overflowCount, 1);
        CHECK_EQ(q.size(), 4U);

        // drain, checking FIFO order
        for(int i = 0; i != 4; ++i) {
            int out = -1;
            CHECK(q.pop_into(out));
            CHECK_EQ(out, i);
        }
        CHECK(q.empty());
        resetOverflow();
    }
}

// ---------------------------------------------------------------------------
// the bulk overloads
static void pushRange() {
    Kvasir::Test::test("pushRange");
    resetOverflow();

    TestQueue<int, 8> q;   // holds 7 elements

    std::array<int, 3> const first{1, 2, 3};
    q.push(first);
    CHECK_EQ(q.size(), 3U);
    CHECK_EQ(q.front(), 1);
    CHECK_EQ(overflowCount, 0);

    std::array<int, 4> const second{4, 5, 6, 7};
    q.push(second);
    CHECK_EQ(q.size(), 7U);
    CHECK_EQ(q.size(), q.max_size());
    CHECK_EQ(overflowCount, 0);

    // the elements come back out in order, across both pushes
    for(int i = 1; i != 8; ++i) {
        int out = -1;
        CHECK(q.pop_into(out));
        CHECK_EQ(out, i);
    }
    CHECK(q.empty());

    // a range that does not fit is rejected as a whole, leaving the queue untouched
    q.push(first);
    resetOverflow();
    std::array<int, 6> const tooBig{1, 2, 3, 4, 5, 6};
    q.push(tooBig);
    CHECK_EQ(overflowCount, 1);
    CHECK_EQ(q.size(), 3U);

    // a range that exactly fills the remaining space is accepted
    resetOverflow();
    std::array<int, 4> const exact{7, 7, 7, 7};
    q.push(exact);
    CHECK_EQ(overflowCount, 0);
    CHECK_EQ(q.size(), 7U);

    // an empty range is a no-op
    resetOverflow();
    q.clear();
    std::vector<int> const empty;
    q.push(empty);
    CHECK_EQ(overflowCount, 0);
    CHECK(q.empty());
}

static void pushRangeWrapsAround() {
    Kvasir::Test::test("pushRangeWrapsAround");
    resetOverflow();

    TestQueue<int, 8> q;

    // advance the indices so that the next bulk push has to wrap
    for(int i = 0; i != 6; ++i) {
        q.push(i);
        int discard = 0;
        CHECK(q.pop_into(discard));
    }
    CHECK(q.empty());

    std::array<int, 5> const values{10, 11, 12, 13, 14};
    q.push(values);
    CHECK_EQ(q.size(), 5U);
    CHECK_EQ(overflowCount, 0);

    for(int i = 0; i != 5; ++i) {
        int out = -1;
        CHECK(q.pop_into(out));
        CHECK_EQ(out, 10 + i);
    }
    CHECK(q.empty());
}

static void popIntoRange() {
    Kvasir::Test::test("popIntoRange");
    resetOverflow();

    TestQueue<int, 8> q;
    for(int i = 0; i != 6; ++i) { q.push(i); }
    CHECK_EQ(q.size(), 6U);

    // a bulk pop only succeeds if the queue holds at least as many elements as the range
    std::array<int, 4> out{};
    CHECK(q.pop_into(out));
    CHECK((out == std::array<int, 4>{0, 1, 2, 3}));
    CHECK_EQ(q.size(), 2U);

    // asking for more than is available fails and consumes nothing
    std::array<int, 4> tooMany{};
    CHECK(!q.pop_into(tooMany));
    CHECK_EQ(q.size(), 2U);

    // exactly the remaining number of elements succeeds
    std::array<int, 2> rest{};
    CHECK(q.pop_into(rest));
    CHECK((rest == std::array<int, 2>{4, 5}));
    CHECK(q.empty());

    // an empty target range trivially succeeds
    std::vector<int> none;
    CHECK(q.pop_into(none));
    CHECK(q.empty());
}

static void popIntoRangeWrapsAround() {
    Kvasir::Test::test("popIntoRangeWrapsAround");
    resetOverflow();

    TestQueue<int, 8> q;

    // push the indices close to the end of the buffer
    for(int i = 0; i != 6; ++i) {
        q.push(i);
        int discard = 0;
        CHECK(q.pop_into(discard));
    }

    std::array<int, 5> const values{20, 21, 22, 23, 24};
    q.push(values);

    std::array<int, 5> out{};
    CHECK(q.pop_into(out));
    CHECK((out == std::array<int, 5>{20, 21, 22, 23, 24}));
    CHECK(q.empty());
}

// ---------------------------------------------------------------------------
// the overflow policy is a template parameter and must be honoured
static void overflowPolicyIgnore() {
    Kvasir::Test::test("overflowPolicyIgnore");

    Queue<int, 3, OverFlowPolicyIgnore> q;   // holds 2 elements
    q.push(1);
    q.push(2);
    CHECK_EQ(q.size(), 2U);

    // silently dropped rather than asserted
    q.push(3);
    CHECK_EQ(q.size(), 2U);

    int out = 0;
    CHECK(q.pop_into(out));
    CHECK_EQ(out, 1);
    CHECK(q.pop_into(out));
    CHECK_EQ(out, 2);
    CHECK(q.empty());
}

// ---------------------------------------------------------------------------
// non trivial element types
static void byteElements() {
    Kvasir::Test::test("byteElements");
    resetOverflow();

    TestQueue<std::uint8_t, 4> q;
    q.push(std::uint8_t{0xAB});
    q.push(std::uint8_t{0xCD});

    std::uint8_t out = 0;
    CHECK(q.pop_into(out));
    CHECK_EQ(out, 0xABU);
    CHECK(q.pop_into(out));
    CHECK_EQ(out, 0xCDU);
    CHECK(q.empty());
}

static void largerQueueUsesWiderIndex() {
    Kvasir::Test::test("largerQueueUsesWiderIndex");
    resetOverflow();

    // Size 300 forces the uint16_t index type; check the arithmetic still works
    TestQueue<int, 300> q;
    CHECK_EQ(q.max_size(), 299U);

    for(int i = 0; i != 299; ++i) { q.push(i); }
    CHECK_EQ(q.size(), 299U);

    resetOverflow();
    q.push(1000);
    CHECK_EQ(overflowCount, 1);

    for(int i = 0; i != 299; ++i) {
        int out = -1;
        CHECK(q.pop_into(out));
        CHECK_EQ(out, i);
    }
    CHECK(q.empty());
}

// ---------------------------------------------------------------------------
// An interleaved producer/consumer run.
//
// This is deliberately single threaded. The queue orders its data accesses against the
// head_/tail_ commits with std::atomic_signal_fence and uses memory_order_relaxed for the
// indices themselves. A signal fence is a compiler barrier only -- it emits no hardware
// barrier -- so the queue is safe for a producer and a consumer that cannot execute
// simultaneously (an interrupt handler and the main loop on a single core, which is what
// the Kvasir::Atomic queue is for) but it establishes no happens-before between two cores.
// Running this as two real threads reports a data race under ThreadSanitizer for exactly
// that reason, so the interleaving is driven by hand instead.
//
// The queue is small so the indices wrap many thousands of times over the run, and the
// producer and consumer are stepped at different rates so the queue spends time full,
// empty and partly filled.
static void interleavedProducerConsumer() {
    Kvasir::Test::test("interleavedProducerConsumer");

    constexpr int Count = 100000;

    Queue<int, 16, OverFlowPolicyIgnore> q;

    int  produced = 0;
    int  received = 0;
    bool ordered  = true;

    while(received != Count) {
        // the producer runs ahead in bursts, filling the queue up to capacity
        for(int burst = 0; burst != 3 && produced != Count; ++burst) {
            if(q.size() == q.max_size()) { break; }
            q.push(produced);
            ++produced;
        }

        // the consumer drains a smaller burst, so the queue keeps filling up
        for(int burst = 0; burst != 2; ++burst) {
            int out = -1;
            if(!q.pop_into(out)) { break; }
            if(out != received) { ordered = false; }
            ++received;
        }
    }

    CHECK(ordered);
    CHECK_EQ(received, Count);
    CHECK_EQ(produced, Count);
    CHECK(q.empty());
}

// the same thing through the bulk overloads, which advance the indices several slots at a
// time and therefore wrap differently
static void interleavedBulkProducerConsumer() {
    Kvasir::Test::test("interleavedBulkProducerConsumer");

    constexpr int Count = 30000;

    Queue<int, 16, OverFlowPolicyIgnore> q;

    int  produced = 0;
    int  received = 0;
    bool ordered  = true;

    while(received != Count) {
        if(produced != Count && q.size() + 5 <= q.max_size()) {
            std::array<int, 5> batch{};
            for(int i = 0; i != 5; ++i) { batch[std::size_t(i)] = produced + i; }
            q.push(batch);
            produced += 5;
        }

        std::array<int, 3> out{};
        if(q.pop_into(out)) {
            for(int v : out) {
                if(v != received) { ordered = false; }
                ++received;
            }
        } else {
            // drain the tail one element at a time once fewer than three are left
            int single = -1;
            if(produced == Count && q.pop_into(single)) {
                if(single != received) { ordered = false; }
                ++received;
            }
        }
    }

    CHECK(ordered);
    CHECK_EQ(received, Count);
    CHECK(q.empty());
}

// the two-run copy and the per-element copy must leave the same queue, from every start and length
static void rangeCopiesInRunsFromEveryIndex() {
    static constexpr std::size_t Size = 13;
    for(std::size_t start = 0; start != Size; ++start) {
        for(std::size_t count = 0; count != Size; ++count) {
            TestQueue<std::uint8_t, Size> runs;
            TestQueue<std::uint8_t, Size> single;
            for(std::size_t i = 0; i != start; ++i) {   // move both indices to `start`
                std::uint8_t out{};
                runs.push(std::uint8_t{0});
                single.push(std::uint8_t{0});
                CHECK(runs.pop_into(out));
                CHECK(single.pop_into(out));
            }
            std::vector<std::uint8_t> in(count);
            for(std::size_t i = 0; i != count; ++i) { in[i] = static_cast<std::uint8_t>(i + 1); }
            std::deque<std::uint8_t> const inSingle(in.begin(), in.end());   // not contiguous
            resetOverflow();
            runs.push(in);
            single.push(inSingle);
            CHECK(overflowCount == 0);
            CHECK(runs.size() == count);
            CHECK(runs.data_ == single.data_);
            CHECK(runs.tail_.load() == single.tail_.load());

            std::vector<std::uint8_t> out(count);
            std::deque<std::uint8_t>  outSingle(count);
            CHECK(runs.pop_into(out));
            CHECK(single.pop_into(outSingle));
            CHECK(out == in);
            CHECK(std::equal(out.begin(), out.end(), outSingle.begin(), outSingle.end()));
            CHECK(runs.empty());
            CHECK(runs.head_.load() == single.head_.load());
        }
    }
}

int main() {
    emptyQueue();
    pushPopSingle();
    pushOnFullQueue();
    popDiscard();
    clearResetsTheQueue();
    indexWrapAround();
    fillDrainAtEveryOffset();

    pushRange();
    pushRangeWrapsAround();
    popIntoRange();
    popIntoRangeWrapsAround();
    rangeCopiesInRunsFromEveryIndex();

    overflowPolicyIgnore();
    byteElements();
    largerQueueUsesWiderIndex();

    interleavedProducerConsumer();
    interleavedBulkProducerConsumer();

    return Kvasir::Test::report();
}
