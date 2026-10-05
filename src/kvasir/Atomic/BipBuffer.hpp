#pragma once
// A bip buffer: a ring that hands out contiguous spans of its own storage. The producer reserves a run, fills it in
// place (a formatter, a DMA channel), commits what it used; the consumer gets the oldest contiguous run, uses it in
// place (a DMA channel, a parser), commits what it used. A reservation that does not fit before the end of the
// storage is taken from the start once the reader has moved far enough; the elements left at the end are skipped
// (wrapEnd_ marks where the data before the wrap stops).
//
//     Kvasir::Atomic::BipBuffer<std::byte, 512> buf{};
//     if(auto s = buf.writeReserve(frameSize); !s.empty()) { encode(s); buf.writeCommit(frameSize); }
//     for(auto s = buf.readReserve(); !s.empty(); s = buf.readReserve()) { buf.readCommit(sink(s)); }
//
// One producer, one consumer; each may be an ISR or the other core (TSync as Queue's). Each index has exactly one
// writer and is only loaded and stored - no read-modify-write, so no exclusives: Armv6-M has none (DDI0419E A3.4),
// and byte, aligned halfword and aligned word accesses are single-copy atomic (A3.5.1).
// A DMA channel as a party: commit before triggering the channel, with a release fence (`dmb`) between them.
#include "kvasir/Atomic/Queue.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>

namespace Kvasir::Atomic {

template<typename T, std::size_t Size, typename TSync = DefaultSync, std::size_t Align = alignof(T)>
struct BipBuffer {
    static_assert(std::is_trivially_copyable_v<T>,
                  "a DMA channel or memcpy moves the elements");
    // Empty with both indices at the end, a wrap may take only n < read_ (n == read_ would read as empty): at most
    // Size - 1 there, and a buffer of one would never take an element again.
    static_assert(Size >= 2,
                  "a bip buffer needs at least two elements");
    using IndexType = Detail::GetIndexTypeT<Size + 1>;   // indices run 0..Size inclusive
    static_assert(std::numeric_limits<IndexType>::max() >= Size,
                  "Size too big");

    static constexpr auto acquire = TSync::load_memory_order;
    static constexpr auto release = TSync::store_memory_order;

    /// Exactly n contiguous elements, or an empty span. Taking a reservation replaces the last one.
    std::span<T> writeReserve(std::size_t n) {
        if(n == 0 || n > Size) { return {}; }
        IndexType const w = write_.load(std::memory_order_relaxed);   // own index
        IndexType const r = read_.load(acquire);   // the reader's progress, and its reads done
        if(w >= r) {                               // data is [r, w), or none
            if(std::size_t{Size} - w >= n) { return open(w, n, false); }
            if(r > n) { return open(0, n, true); }   // wrap: [0, n) with n < r keeps w != r
        } else if(std::size_t{r} - w > n) {          // wrapped: data is [r, wrapEnd) + [0, w)
            return open(w, n, false);
        }
        return {};
    }

    /// The largest run free now at the write position, or after the wrap when nothing is left before the end.
    std::span<T> writeReserveMax() {
        IndexType const w = write_.load(std::memory_order_relaxed);
        IndexType const r = read_.load(acquire);
        if(w >= r) {
            if(w != Size) { return open(w, static_cast<std::size_t>(Size - w), false); }
            if(r > 1) { return open(0, static_cast<std::size_t>(r - 1), true); }
            return {};
        }
        if(r - w > 1) { return open(w, static_cast<std::size_t>(r - w - 1), false); }
        return {};
    }

    /// Publish the first k elements of the last reservation (k <= its size); k == 0 publishes nothing.
    void writeCommit(std::size_t k) {
        if(k == 0) { return; }   // a wrap with nothing in it never happened
        if(k > resLen_) { k = resLen_; }
        if(resWraps_) {
            // the data before the wrap ends here; ordered by the release below
            wrapEnd_.store(write_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        }
        TSync::fence();
        write_.store(static_cast<IndexType>(resAt_ + k), release);
        resLen_ = 0;
    }

    /// Copy in as much of `in` as fits, in up to two runs; how many were taken.
    std::size_t write(std::span<T const> in) {
        std::size_t done = 0;
        for(int run = 0; run != 2 && done != in.size(); ++run) {
            auto const s = writeReserveMax();
            auto const n = std::min(s.size(), in.size() - done);
            if(n == 0) { break; }
            std::copy_n(in.data() + done, n, s.data());
            writeCommit(n);
            done += n;
        }
        return done;
    }

    /// The oldest contiguous run of committed elements (empty when there is none).
    std::span<T const> readReserve() {
        auto const s = reserveRead();
        return {s.data(), s.size()};
    }

    /// The same, writable: a consumer that decodes in place.
    std::span<T> readReserveMutable() { return reserveRead(); }

    /// Free the first k elements of the last read reservation (k <= its size).
    void readCommit(std::size_t k) {
        if(k > static_cast<std::size_t>(rdEnd_ - rdAt_)) {
            k = static_cast<std::size_t>(rdEnd_ - rdAt_);
        }
        auto next = static_cast<IndexType>(rdAt_ + k);
        // Eager wrap: the run before the wrap used up -> read_ goes to 0 at once, so the producer sees [w, Size) free.
        if(rdBeforeWrap_ && next == rdEnd_) { next = 0; }
        rdAt_  = next;
        rdEnd_ = next;   // the reservation is used: a second commit frees nothing
        TSync::fence();
        read_.store(next, release);
    }

    /// Committed elements not yet read: a snapshot.
    [[nodiscard]] std::size_t size() const {
        IndexType const w = write_.load(acquire);
        IndexType const r = read_.load(acquire);
        if(w >= r) { return static_cast<std::size_t>(w - r); }
        return static_cast<std::size_t>(wrapEnd_.load(std::memory_order_relaxed) - r) + w;
    }

    [[nodiscard]] bool empty() const { return size() == 0; }

    [[nodiscard]] static constexpr std::size_t capacity() { return Size; }

    /// Only with both sides stopped (as Queue's).
    void clear() {
        write_.store(0, std::memory_order_relaxed);
        wrapEnd_.store(0, std::memory_order_relaxed);
        read_.store(0, std::memory_order_relaxed);
        resLen_       = 0;
        rdAt_         = 0;
        rdEnd_        = 0;
        rdBeforeWrap_ = false;
    }

private:
    std::span<T> open(IndexType   at,
                      std::size_t n,
                      bool        wraps) {
        resAt_    = at;
        resLen_   = static_cast<IndexType>(n);
        resWraps_ = wraps;
        return {data_.data() + at, n};
    }

    std::span<T> reserveRead() {
        IndexType       r   = read_.load(std::memory_order_relaxed);
        IndexType const w   = write_.load(acquire);   // and the data before it
        IndexType       end = w;
        rdBeforeWrap_       = false;
        if(r > w) {   // the producer has wrapped
            IndexType const we = wrapEnd_.load(std::memory_order_relaxed);   // stored before write_
            if(r == we) {
                r = 0;
            } else {
                end           = we;
                rdBeforeWrap_ = true;
            }
        }
        rdAt_  = r;
        rdEnd_ = end;
        return {data_.data() + r, static_cast<std::size_t>(end - r)};
    }

    alignas(Align) std::array<T,
                              Size> data_{};
    std::atomic<IndexType> write_{};
    std::atomic<IndexType> wrapEnd_{};
    std::atomic<IndexType> read_{};
    // producer-private: the open write reservation
    IndexType resAt_{};
    IndexType resLen_{};
    bool      resWraps_{};
    // consumer-private: the open read reservation
    IndexType rdAt_{};
    IndexType rdEnd_{};
    bool      rdBeforeWrap_{};
};
}   // namespace Kvasir::Atomic
