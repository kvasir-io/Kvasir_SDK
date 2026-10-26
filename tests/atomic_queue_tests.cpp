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

using Kvasir::Atomic::Fifo;
using Kvasir::Atomic::OverflowPolicyIgnore;
using Kvasir::Atomic::Queue;

// the queue's state is private; the friend lets a test compare two queues slot for slot
namespace Kvasir::Atomic {
struct QueueTestAccess {
    template<typename Q>
    static auto const& slots(Q const& q) {
        return q.data_;
    }

    template<typename Q>
    static auto head(Q const& q) {
        return q.head_.load();
    }

    template<typename Q>
    static auto tail(Q const& q) {
        return q.tail_.load();
    }
};
}   // namespace Kvasir::Atomic

using Kvasir::Atomic::QueueTestAccess;

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
// the index type is the smallest one that holds every stored index, 0 .. Size - 1
static_assert(std::is_same_v<Queue<int,
                                   4>::IndexType,
                             std::uint8_t>);
static_assert(std::is_same_v<Queue<int,
                                   256>::IndexType,
                             std::uint8_t>);
static_assert(std::is_same_v<Queue<int,
                                   257>::IndexType,
                             std::uint16_t>);
static_assert(std::is_same_v<Queue<int,
                                   65536>::IndexType,
                             std::uint16_t>);
static_assert(std::is_same_v<Queue<int,
                                   65537>::IndexType,
                             std::uint32_t>);

// Fifo is named by what it holds: the same type as the Queue one slot larger
static_assert(std::is_same_v<Fifo<int,
                                  8>,
                             Queue<int,
                                   9>>);
static_assert(std::is_same_v<Fifo<int,
                                  8,
                                  OverflowPolicyIgnore,
                                  Kvasir::Atomic::SyncThread>,
                             Queue<int,
                                   9,
                                   OverflowPolicyIgnore,
                                   Kvasir::Atomic::SyncThread>>);
static_assert(Fifo<int,
                   8>::capacity()
              == 8);
static_assert(Queue<int,
                    4>::capacity()
              == 3);
static_assert(Fifo<int,
                   254>::capacity()
              == 254);   // a Queue<int, 255>: still uint8_t indices
static_assert(std::is_same_v<Fifo<int,
                                  255>::IndexType,
                             std::uint8_t>);   // a Queue<int, 256>: indices up to 255

namespace {
// ---------------------------------------------------------------------------
void emptyQueue() {
    Kvasir::Test::test("emptyQueue");
    resetOverflow();

    TestQueue<int, 4> q;
    CHECK(q.empty());
    CHECK_EQ(q.size(), 0U);
    CHECK_EQ(q.max_size(), 3U);   // one slot is reserved to distinguish full from empty

    int out = 42;
    CHECK(!q.popInto(out));
    CHECK_EQ(out, 42);   // a failed pop leaves the target alone

    // popping an empty queue is a no-op rather than an error
    q.pop();
    CHECK(q.empty());
    CHECK_EQ(overflowCount, 0);

    // peek() says "empty" without calling the policy
    CHECK(q.peek() == nullptr);
    CHECK_EQ(overflowCount, 0);

    // front() on an empty queue breaks its precondition: it reports through the overflow policy
    // (and, with a policy that returns, the slot it hands out is stale)
    (void)q.front();
    CHECK_EQ(overflowCount, 1);
}

void peekSeesTheOldest() {
    Kvasir::Test::test("peekSeesTheOldest");
    resetOverflow();

    TestQueue<int, 4> q;
    q.push(7);
    q.push(8);
    int const* p = q.peek();
    CHECK(p != nullptr);
    CHECK(p == &q.front());   // the same slot front() refers to
    CHECK_EQ(*p, 7);
    CHECK_EQ(q.size(), 2U);   // peek does not consume

    q.pop();
    p = q.peek();
    CHECK(p != nullptr);
    CHECK_EQ(*p, 8);
    q.pop();
    CHECK(q.peek() == nullptr);

    // across the wrap: the index runs 0..3 several times
    for(int i = 0; i != 20; ++i) {
        q.push(i);
        CHECK(q.peek() != nullptr);
        CHECK_EQ(*q.peek(), i);
        q.pop();
        CHECK(q.peek() == nullptr);
    }
    CHECK_EQ(overflowCount, 0);
}

void fifoHoldsItsCapacity() {
    Kvasir::Test::test("fifoHoldsItsCapacity");
    resetOverflow();

    Fifo<int, 5, CountingOverflowPolicy> q;
    CHECK_EQ(q.capacity(), 5U);
    CHECK_EQ(q.max_size(), q.capacity());
    for(int i = 0; i != 5; ++i) { q.push(i); }
    CHECK_EQ(q.size(), 5U);
    CHECK_EQ(overflowCount, 0);
    q.push(99);   // the sixth does not fit
    CHECK_EQ(overflowCount, 1);
    for(int i = 0; i != 5; ++i) {
        int out = -1;
        CHECK(q.popInto(out));
        CHECK_EQ(out, i);
    }
    CHECK(q.empty());
}

// Size 255 did not compile before (the index assert wanted max() > Size); it holds 254 with uint8_t indices
void fullQueueOfSize255() {
    Kvasir::Test::test("fullQueueOfSize255");
    resetOverflow();

    TestQueue<std::uint8_t, 255> q;
    static_assert(std::is_same_v<decltype(q)::IndexType, std::uint8_t>);
    CHECK_EQ(q.max_size(), 254U);
    CHECK_EQ(q.capacity(), 254U);

    for(std::size_t i = 0; i != 254; ++i) { q.push(static_cast<std::uint8_t>(i)); }
    CHECK_EQ(q.size(), 254U);
    CHECK_EQ(overflowCount, 0);
    q.push(std::uint8_t{0xFF});
    CHECK_EQ(overflowCount, 1);
    CHECK_EQ(q.size(), 254U);

    for(std::size_t i = 0; i != 254; ++i) {
        std::uint8_t out = 0;
        CHECK(q.popInto(out));
        CHECK_EQ(out, i);
    }
    CHECK(q.empty());

    // the indices wrap at 255, a value the old assert ruled out
    for(std::size_t i = 0; i != 1000; ++i) {
        q.push(static_cast<std::uint8_t>(i));
        std::uint8_t out = 0;
        CHECK(q.popInto(out));
        CHECK_EQ(out, static_cast<std::uint8_t>(i));
    }

    // the range overloads across the wrap, filled to the brim
    std::vector<std::uint8_t> in(254);
    for(std::size_t i = 0; i != in.size(); ++i) { in[i] = static_cast<std::uint8_t>(i ^ 0x5A); }
    q.push(in);
    CHECK_EQ(overflowCount, 1);
    CHECK_EQ(q.size(), 254U);
    std::vector<std::uint8_t> out(254);
    CHECK(q.popInto(out));
    CHECK(out == in);
    CHECK(q.empty());
    CHECK_EQ(overflowCount, 1);
}

// Size 256: uint8_t indices that reach 255, wrapped by the power-of-two mask (was uint16_t)
void fullQueueOfSize256() {
    Kvasir::Test::test("fullQueueOfSize256");
    resetOverflow();

    TestQueue<std::uint8_t, 256> q;
    static_assert(std::is_same_v<decltype(q)::IndexType, std::uint8_t>);
    CHECK_EQ(q.capacity(), 255U);
    for(std::size_t i = 0; i != 255; ++i) { q.push(static_cast<std::uint8_t>(i)); }
    CHECK_EQ(q.size(), 255U);
    q.push(std::uint8_t{0});
    CHECK_EQ(overflowCount, 1);
    for(std::size_t i = 0; i != 255; ++i) {
        std::uint8_t out = 0;
        CHECK(q.popInto(out));
        CHECK_EQ(out, i);
    }
    CHECK(q.empty());
    for(std::size_t i = 0; i != 1000; ++i) {
        q.push(static_cast<std::uint8_t>(i));
        std::uint8_t out = 0;
        CHECK(q.popInto(out));
        CHECK_EQ(out, static_cast<std::uint8_t>(i));
    }
    std::vector<std::uint8_t> in(255);
    for(std::size_t i = 0; i != in.size(); ++i) { in[i] = static_cast<std::uint8_t>(i ^ 0xA5); }
    q.push(in);
    CHECK_EQ(q.size(), 255U);
    std::vector<std::uint8_t> out(255);
    CHECK(q.popInto(out));
    CHECK(out == in);
    CHECK_EQ(overflowCount, 1);
}

// with 65535 slots the indices are uint16_t and reach 65534
void fullQueueOfSize65535() {
    Kvasir::Test::test("fullQueueOfSize65535");
    resetOverflow();

    static TestQueue<std::uint16_t, 65535> q;   // 128 KiB: static, not on the stack
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(q)>::IndexType, std::uint16_t>);
    for(std::size_t i = 0; i != 65534; ++i) { q.push(static_cast<std::uint16_t>(i)); }
    CHECK_EQ(q.size(), 65534U);
    q.push(std::uint16_t{0});
    CHECK_EQ(overflowCount, 1);
    bool inOrder = true;
    for(std::size_t i = 0; i != 65534; ++i) {
        std::uint16_t out = 0;
        inOrder           = q.popInto(out) && out == i && inOrder;
    }
    CHECK(inOrder);
    CHECK(q.empty());
    q.push(std::uint16_t{42});   // index 65534 -> 0
    CHECK_EQ(*q.peek(), 42U);
}

void pushPopSingle() {
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
    CHECK(q.popInto(out));
    CHECK_EQ(out, 1);
    CHECK(q.popInto(out));
    CHECK_EQ(out, 2);
    CHECK_EQ(q.size(), 1U);
    CHECK(q.popInto(out));
    CHECK_EQ(out, 3);
    CHECK(q.empty());
    CHECK(!q.popInto(out));

    CHECK_EQ(overflowCount, 0);
}

void pushOnFullQueue() {
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
    CHECK(q.popInto(out));
    CHECK_EQ(out, 1);

    // after making room a push succeeds again
    resetOverflow();
    q.push(5);
    CHECK_EQ(overflowCount, 0);
    CHECK_EQ(q.size(), 3U);
}

void popDiscard() {
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

void clearResetsTheQueue() {
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
void indexWrapAround() {
    Kvasir::Test::test("indexWrapAround");
    resetOverflow();

    TestQueue<int, 4> q;

    // many more iterations than the buffer has slots, so the indices wrap repeatedly
    for(int i = 0; i != 100; ++i) {
        q.push(i);
        CHECK_EQ(q.size(), 1U);

        int out = -1;
        CHECK(q.popInto(out));
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
        CHECK(q.popInto(out));
        CHECK_EQ(out, i - 2);
        CHECK_EQ(q.size(), 2U);
    }
    CHECK_EQ(overflowCount, 0);
}

// filling to capacity, draining and refilling, at every possible starting offset
void fillDrainAtEveryOffset() {
    Kvasir::Test::test("fillDrainAtEveryOffset");
    resetOverflow();

    TestQueue<int, 5> q;   // holds 4 elements

    for(int offset = 0; offset != 5; ++offset) {
        // move the start index along by pushing and popping `offset` times
        for(int i = 0; i != offset; ++i) {
            q.push(-1);
            int discard = 0;
            CHECK(q.popInto(discard));
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
            CHECK(q.popInto(out));
            CHECK_EQ(out, i);
        }
        CHECK(q.empty());
        resetOverflow();
    }
}

// ---------------------------------------------------------------------------
// the bulk overloads
void pushRange() {
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
        CHECK(q.popInto(out));
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

void pushRangeWrapsAround() {
    Kvasir::Test::test("pushRangeWrapsAround");
    resetOverflow();

    TestQueue<int, 8> q;

    // advance the indices so that the next bulk push has to wrap
    for(int i = 0; i != 6; ++i) {
        q.push(i);
        int discard = 0;
        CHECK(q.popInto(discard));
    }
    CHECK(q.empty());

    std::array<int, 5> const values{10, 11, 12, 13, 14};
    q.push(values);
    CHECK_EQ(q.size(), 5U);
    CHECK_EQ(overflowCount, 0);

    for(int i = 0; i != 5; ++i) {
        int out = -1;
        CHECK(q.popInto(out));
        CHECK_EQ(out, 10 + i);
    }
    CHECK(q.empty());
}

void popIntoRange() {
    Kvasir::Test::test("popIntoRange");
    resetOverflow();

    TestQueue<int, 8> q;
    for(int i = 0; i != 6; ++i) { q.push(i); }
    CHECK_EQ(q.size(), 6U);

    // a bulk pop only succeeds if the queue holds at least as many elements as the range
    std::array<int, 4> out{};
    CHECK(q.popInto(out));
    CHECK((out == std::array<int, 4>{0, 1, 2, 3}));
    CHECK_EQ(q.size(), 2U);

    // asking for more than is available fails and consumes nothing
    std::array<int, 4> tooMany{};
    CHECK(!q.popInto(tooMany));
    CHECK_EQ(q.size(), 2U);

    // exactly the remaining number of elements succeeds
    std::array<int, 2> rest{};
    CHECK(q.popInto(rest));
    CHECK((rest == std::array<int, 2>{4, 5}));
    CHECK(q.empty());

    // an empty target range trivially succeeds
    std::vector<int> none;
    CHECK(q.popInto(none));
    CHECK(q.empty());
}

void popIntoRangeWrapsAround() {
    Kvasir::Test::test("popIntoRangeWrapsAround");
    resetOverflow();

    TestQueue<int, 8> q;

    // push the indices close to the end of the buffer
    for(int i = 0; i != 6; ++i) {
        q.push(i);
        int discard = 0;
        CHECK(q.popInto(discard));
    }

    std::array<int, 5> const values{20, 21, 22, 23, 24};
    q.push(values);

    std::array<int, 5> out{};
    CHECK(q.popInto(out));
    CHECK((out == std::array<int, 5>{20, 21, 22, 23, 24}));
    CHECK(q.empty());
}

// ---------------------------------------------------------------------------
// the overflow policy is a template parameter and must be honoured
void overflowPolicyIgnore() {
    Kvasir::Test::test("overflowPolicyIgnore");

    Queue<int, 3, OverflowPolicyIgnore> q;   // holds 2 elements
    q.push(1);
    q.push(2);
    CHECK_EQ(q.size(), 2U);

    // silently dropped rather than asserted
    q.push(3);
    CHECK_EQ(q.size(), 2U);

    int out = 0;
    CHECK(q.popInto(out));
    CHECK_EQ(out, 1);
    CHECK(q.popInto(out));
    CHECK_EQ(out, 2);
    CHECK(q.empty());
}

// ---------------------------------------------------------------------------
// non trivial element types
void byteElements() {
    Kvasir::Test::test("byteElements");
    resetOverflow();

    TestQueue<std::uint8_t, 4> q;
    q.push(std::uint8_t{0xAB});
    q.push(std::uint8_t{0xCD});

    std::uint8_t out = 0;
    CHECK(q.popInto(out));
    CHECK_EQ(out, 0xABU);
    CHECK(q.popInto(out));
    CHECK_EQ(out, 0xCDU);
    CHECK(q.empty());
}

void largerQueueUsesWiderIndex() {
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
        CHECK(q.popInto(out));
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
void interleavedProducerConsumer() {
    Kvasir::Test::test("interleavedProducerConsumer");

    constexpr int count = 100000;

    Queue<int, 16, OverflowPolicyIgnore> q;

    int  produced = 0;
    int  received = 0;
    bool ordered  = true;

    while(received != count) {
        // the producer runs ahead in bursts, filling the queue up to capacity
        for(int burst = 0; burst != 3 && produced != count; ++burst) {
            if(q.size() == q.max_size()) { break; }
            q.push(produced);
            ++produced;
        }

        // the consumer drains a smaller burst, so the queue keeps filling up
        for(int burst = 0; burst != 2; ++burst) {
            int out = -1;
            if(!q.popInto(out)) { break; }
            if(out != received) { ordered = false; }
            ++received;
        }
    }

    CHECK(ordered);
    CHECK_EQ(received, count);
    CHECK_EQ(produced, count);
    CHECK(q.empty());
}

// the same thing through the bulk overloads, which advance the indices several slots at a
// time and therefore wrap differently
void interleavedBulkProducerConsumer() {
    Kvasir::Test::test("interleavedBulkProducerConsumer");

    constexpr int count = 30000;

    Queue<int, 16, OverflowPolicyIgnore> q;

    int  produced = 0;
    int  received = 0;
    bool ordered  = true;

    while(received != count) {
        if(produced != count && q.size() + 5 <= q.max_size()) {
            std::array<int, 5> batch{};
            for(int i = 0; i != 5; ++i) { batch[static_cast<std::size_t>(i)] = produced + i; }
            q.push(batch);
            produced += 5;
        }

        std::array<int, 3> out{};
        if(q.popInto(out)) {
            for(int const v : out) {
                if(v != received) { ordered = false; }
                ++received;
            }
        } else {
            // drain the tail one element at a time once fewer than three are left
            int single = -1;
            if(produced == count && q.popInto(single)) {
                if(single != received) { ordered = false; }
                ++received;
            }
        }
    }

    CHECK(ordered);
    CHECK_EQ(received, count);
    CHECK(q.empty());
}

// the two-run copy and the per-element copy must leave the same queue, from every start and length
void rangeCopiesInRunsFromEveryIndex() {
    static constexpr std::size_t size = 13;
    for(std::size_t start = 0; start != size; ++start) {
        for(std::size_t count = 0; count != size; ++count) {
            TestQueue<std::uint8_t, size> runs;
            TestQueue<std::uint8_t, size> single;
            for(std::size_t i = 0; i != start; ++i) {   // move both indices to `start`
                std::uint8_t out{};
                runs.push(std::uint8_t{0});
                single.push(std::uint8_t{0});
                CHECK(runs.popInto(out));
                CHECK(single.popInto(out));
            }
            std::vector<std::uint8_t> in(count);
            for(std::size_t i = 0; i != count; ++i) { in[i] = static_cast<std::uint8_t>(i + 1); }
            std::deque<std::uint8_t> const inSingle(in.begin(), in.end());   // not contiguous
            resetOverflow();
            runs.push(in);
            single.push(inSingle);
            CHECK(overflowCount == 0);
            CHECK(runs.size() == count);
            CHECK(QueueTestAccess::slots(runs) == QueueTestAccess::slots(single));
            CHECK(QueueTestAccess::tail(runs) == QueueTestAccess::tail(single));

            std::vector<std::uint8_t> out(count);
            std::deque<std::uint8_t>  outSingle(count);
            CHECK(runs.popInto(out));
            CHECK(single.popInto(outSingle));
            CHECK(out == in);
            CHECK(std::ranges::equal(out, outSingle));
            CHECK(runs.empty());
            CHECK(QueueTestAccess::head(runs) == QueueTestAccess::head(single));
        }
    }
}
}   // namespace

int main() {
    emptyQueue();
    peekSeesTheOldest();
    fifoHoldsItsCapacity();
    fullQueueOfSize255();
    fullQueueOfSize256();
    fullQueueOfSize65535();
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
