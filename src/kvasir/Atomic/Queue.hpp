#pragma once

#include "kvasir/Mpl/Utility.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <ranges>
#include <type_traits>

namespace Kvasir { namespace Atomic {

    // OverFlowPolicyAssert is the default action which is taken if
    // an overflow occurs. The user is encurraged to provide their
    // own policy which call reset or some other error handler
    struct OverFlowPolicyAssert {
        [[noreturn]] void operator()() { assert(false); }
    };

    struct OverFlowPolicyIgnore {
        void operator()() {}
    };

    // How a Queue orders its commit against the data it commits.
    //
    // SyncSignal is the original: relaxed index accesses and a compiler-only fence. Exact
    // for one core (the producer and consumer are the same core's thread and ISR, and a
    // core never reorders its own memory accesses against an interrupt). Free.
    //
    // SyncThread makes the commit a release and the peek an acquire, one dmb each on
    // Armv8-M, so a consumer on the other core sees the data the index promises.
    struct SyncSignal {
        static constexpr auto load_memory_order{std::memory_order_relaxed};
        static constexpr auto store_memory_order{std::memory_order_relaxed};

        static void fence() { std::atomic_signal_fence(std::memory_order_release); }
    };

    struct SyncThread {
        static constexpr auto load_memory_order{std::memory_order_acquire};
        static constexpr auto store_memory_order{std::memory_order_release};

        static void fence() { std::atomic_signal_fence(std::memory_order_release); }
    };

    // A multicore build (CORE1_STACK_SIZE set, KVASIR_MULTICORE defined) gets cross-core
    // correct queues by default; a single-core build keeps its exact old code.
#if defined(KVASIR_MULTICORE) && KVASIR_MULTICORE
    using DefaultSync = SyncThread;
#else
    using DefaultSync = SyncSignal;
#endif

    namespace Detail {
        using namespace MPL;

        template<std::size_t Size, typename = void>
        struct GetIndexType {
            using type = std::uint32_t;
        };

        template<std::size_t Size>
        struct GetIndexType<Size, EnableIfT<(Size <= 255)>> {
            using type = std::uint8_t;
        };

        template<std::size_t Size>
        struct GetIndexType<Size, EnableIfT<(Size > 255 && Size <= 65535)>> {
            using type = std::uint16_t;
        };

        template<std::size_t Size>
        using GetIndexTypeT = typename GetIndexType<Size, void>::type;

    }   // namespace Detail

    template<typename TDataType,
             std::size_t Size,
             typename TOverflowPolicy = OverFlowPolicyAssert,
             typename TSync           = DefaultSync>
    struct Queue {
        using IndexType = Detail::GetIndexTypeT<Size>;
        static_assert(std::numeric_limits<IndexType>::max() > Size,
                      "Size to big");
        static constexpr auto       load_memory_order{TSync::load_memory_order};
        static constexpr auto       store_memory_order{TSync::store_memory_order};
        std::atomic<IndexType>      head_{};
        std::atomic<IndexType>      tail_{};
        std::array<TDataType, Size> data_{};

        static constexpr IndexType distance(IndexType head,
                                            IndexType tail) {
            auto const t = static_cast<std::size_t>(tail);
            auto const h = static_cast<std::size_t>(head);
            return static_cast<IndexType>(t >= h ? t - h : Size - h + t);
        }

        static constexpr IndexType next(IndexType in) {
            return static_cast<IndexType>((static_cast<std::size_t>(in) + 1) % Size);
        }

        // contiguous trivially copyable ranges are copied in at most two runs instead of per element
        template<typename TRange>
        static constexpr bool CopiesInRuns
          = std::ranges::contiguous_range<TRange> && std::is_trivially_copyable_v<TDataType>;

        static constexpr IndexType advanced(IndexType   index,
                                            std::size_t count) {
            std::size_t const end = std::size_t{index} + count;
            return IndexType(end >= Size ? end - Size : end);
        }

        void push(TDataType in) {
            auto const tail     = tail_.load(load_memory_order);
            auto const head     = head_.load(load_memory_order);
            auto       nextTail = next(tail);
            if(head != nextTail) {
                data_[tail] = in;   //NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
                TSync::fence();
                tail_.store(nextTail, store_memory_order);   // commit
            } else {
                TOverflowPolicy{}();
            }
        }

        template<typename TRange,
                 typename = std::enable_if_t<
                   std::is_same<std::decay_t<decltype(*std::declval<TRange>().begin())>,
                                TDataType>::value>>
        void push(TRange const& range) {
            auto       tail = tail_.load(load_memory_order);
            auto const head = head_.load(load_memory_order);
            if(range.size() < Size - distance(head, tail)) {
                if constexpr(CopiesInRuns<TRange const>) {
                    std::size_t const count = range.size();
                    std::size_t const first = std::min<std::size_t>(count, Size - tail);
                    std::copy_n(std::ranges::data(range), first, data_.data() + tail);
                    std::copy_n(std::ranges::data(range) + first, count - first, data_.data());
                    tail = advanced(tail, count);
                } else {
                    auto       begin = range.begin();
                    auto const end   = range.end();
                    while(begin != end) {
                        data_[tail]
                          = *begin++;   //NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
                        tail = next(tail);
                    }
                }
                TSync::fence();
                tail_.store(tail, store_memory_order);   // commit
            } else {
                TOverflowPolicy{}();
            }
        }

        bool pop_into(TDataType& out) {
            auto const tail = tail_.load(load_memory_order);
            auto const head = head_.load(load_memory_order);
            if(head == tail) { return false; }
            out = data_[head];   //NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
            TSync::fence();
            head_.store(next(head), store_memory_order);   // commit
            return true;
        }

        template<typename TRange,
                 typename = std::enable_if_t<
                   std::is_same<std::decay_t<decltype(*std::declval<TRange>().begin())>,
                                TDataType>::value>>
        bool pop_into(TRange& range) {
            auto const tail  = tail_.load(load_memory_order);
            auto       head  = head_.load(load_memory_order);
            auto const lsize = distance(head, tail);
            if(lsize < range.size()) { return false; }
            if constexpr(CopiesInRuns<TRange>) {
                std::size_t const count = range.size();
                std::size_t const first = std::min<std::size_t>(count, Size - head);
                std::copy_n(data_.data() + head, first, std::ranges::data(range));
                std::copy_n(data_.data(), count - first, std::ranges::data(range) + first);
                head = advanced(head, count);
            } else {
                auto       begin = range.begin();
                auto const end   = range.end();
                while(begin != end) {
                    *begin++
                      = data_[head];   //NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
                    head = next(head);
                }
            }
            TSync::fence();
            head_.store(head, store_memory_order);   // commit
            return true;
        }

        void pop() {
            auto const tail = tail_.load(load_memory_order);
            auto const head = head_.load(load_memory_order);
            if(head == tail) { return; }
            head_.store(next(head), store_memory_order);   // commit
        }

        TDataType const& front() const {
            auto const head = head_.load(load_memory_order);
            if(head == tail_.load(load_memory_order)) { TOverflowPolicy{}(); }
            TDataType const& ret
              = data_[head];   //NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
            return ret;
        }

        std::size_t size() const {
            return distance(head_.load(load_memory_order), tail_.load(load_memory_order));
        }

        bool empty() const { return size() == 0; }

        constexpr std::size_t max_size() const { return Size - 1; }

        // Drop everything. Not concurrency-safe: for a producer or consumer that is provably
        // stopped (a SecondaryCore's primaryPrepare()), never while both are live.
        void clear() {
            head_.store(0, store_memory_order);
            tail_.store(0, store_memory_order);
        }
    };
}}   // namespace Kvasir::Atomic
