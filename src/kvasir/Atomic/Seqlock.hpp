#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <type_traits>

// The payload copy races with the writer on purpose; the sequence number is what makes it
// safe. ThreadSanitizer cannot know that: under TSan the copy is done word by word through
// relaxed atomics, which models exactly the guarantee the seqlock needs (every word is a
// whole word) and lets TSan check the protocol around it. Everywhere else it is a memcpy.
#if defined(__SANITIZE_THREAD__)
    #define KVASIR_SEQLOCK_TSAN 1
#elif defined(__has_feature)
    #if __has_feature(thread_sanitizer)
        #define KVASIR_SEQLOCK_TSAN 1
    #endif
#endif
#ifndef KVASIR_SEQLOCK_TSAN
    #define KVASIR_SEQLOCK_TSAN 0
#endif

namespace Kvasir::Atomic {

namespace detail {
    // dst <- src, sizeof(T) bytes.
    template<typename T>
    void seqlockCopy(T&       dst,
                     T const& src) {
#if KVASIR_SEQLOCK_TSAN
        // atomic_ref<T const> is C++26 (P3323); libc++ does not have it yet. The referenced
        // object is never const itself (a Seqlock member or the caller's value), so
        // casting the constness away for the relaxed load is well defined.
        auto* d = reinterpret_cast<unsigned char*>(&dst);
        auto* s = const_cast<unsigned char*>(reinterpret_cast<unsigned char const*>(&src));
        for(std::size_t i = 0; i < sizeof(T); ++i) {
            std::atomic_ref<unsigned char>{d[i]}.store(
              std::atomic_ref<unsigned char>{s[i]}.load(std::memory_order_relaxed),
              std::memory_order_relaxed);
        }
#else
        std::memcpy(&dst, &src, sizeof(T));
#endif
    }
}   // namespace detail

// A latest-value mailbox: one writer publishes whole snapshots of T, any number of readers
// take the most recent one. The writer never waits, never blocks and never fails; a reader
// retries if a publish landed while it was copying. That is the right shape for "core 0
// simulates, core 1 draws the newest state it can get": a stale frame is fine, a torn one
// is not, and the producer must never stall on the consumer.
//
// Classic sequence lock. The counter is odd while a write is in flight; a reader that saw
// the same even value before and after its copy has a consistent snapshot. T must be
// trivially copyable, because the copy is done with the counter racing it by design.
template<typename T>
struct Seqlock {
    static_assert(std::is_trivially_copyable_v<T>,
                  "Seqlock copies T while it is being written");

    void publish(T const& value) {
        auto const s = seq_.load(std::memory_order_relaxed);
        seq_.store(s + 1, std::memory_order_relaxed);
        // The fence keeps the "in flight" mark ahead of the data stores; the release store
        // below keeps the data stores ahead of the "done" mark.
        std::atomic_thread_fence(std::memory_order_release);
        detail::seqlockCopy(data_, value);
        seq_.store(s + 2, std::memory_order_release);
    }

    // One attempt. False if a publish overlapped the copy; `out` is then garbage.
    [[nodiscard]] bool tryRead(T& out) const {
        auto const s1 = seq_.load(std::memory_order_acquire);
        if((s1 & 1U) != 0) { return false; }
        detail::seqlockCopy(out, data_);
        std::atomic_thread_fence(std::memory_order_acquire);
        return seq_.load(std::memory_order_relaxed) == s1;
    }

    // Up to `attempts` tries. False if none of them saw a quiet counter; `out` is then
    // garbage. This is the read for a writer on the other core: a core that halts (fault,
    // breakpoint, debugger, reset) between its two counter stores leaves the counter odd
    // for good, and a reader that spins on it is gone with it. A few hundred attempts is
    // far more than a live writer ever needs, and the caller keeps its previous snapshot.
    [[nodiscard]] bool tryRead(T&       out,
                               unsigned attempts) const {
        for(unsigned i = 0; i < attempts; ++i) {
            if(tryRead(out)) { return true; }
        }
        return false;
    }

    // Spin until a consistent snapshot is read. Bounded in practice by the writer's rate:
    // a copy of a kilobyte loses to a publish only if publishes come faster than copies.
    // Only for a writer that cannot stop mid-publish (same core, or the caller accepts a
    // hang if the other core dies); otherwise use the bounded tryRead().
    [[nodiscard]] T read() const {
        T out{};
        while(!tryRead(out)) {}
        return out;
    }

    // Back to "never published". Only while the writer is provably not running, which is
    // what a SecondaryCore's primaryPrepare() hook is: a core reset mid-publish leaves the
    // counter odd, and this is the only way out of that.
    void reset() {
        data_ = T{};
        seq_.store(0, std::memory_order_release);
    }

    // Even, and increments by two per publish. Cheap way to see "has anything changed".
    [[nodiscard]] std::uint32_t version() const { return seq_.load(std::memory_order_acquire); }

private:
    std::atomic<std::uint32_t> seq_{0};
    T                          data_{};
};

}   // namespace Kvasir::Atomic
