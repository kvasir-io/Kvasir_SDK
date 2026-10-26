#pragma once
#include "kvasir/Atomic/Sync.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <ranges>
#include <type_traits>

namespace Kvasir { namespace Atomic {

    // The default action on a push into a full queue. It never returns: an assertion failure in a
    // build with asserts, a trap with NDEBUG (returning from a [[noreturn]] function would be
    // undefined behaviour). The trap is not a Kvasir panic because Atomic does not depend on Util; on a
    // target it ends in the fault handler. A policy of your own may log, count or reset instead.
    struct OverflowPolicyAssert {
        [[noreturn]] void operator()() {
#ifdef NDEBUG
            __builtin_trap();
#else
            assert(false);
#endif
        }
    };

    struct OverflowPolicyIgnore {
        void operator()() {}
    };

    // How a Queue orders its commit against the data it commits: SyncSignal, SyncThread, DefaultSync (Sync.hpp).

    namespace Detail {
        // The slots of a queue of trivially copyable elements: plain bytes, all zero at start.
        // A slot holds an element only between its push and its pop, so it needs no initial
        // value - but as a std::array<T> the slots are value-initialised, and a T with default
        // member values (a request's default timeout, an empty callback) then makes the whole
        // queue an initialised object: stored in the flash image and copied at start-up
        // (an I2C request queue: 3332 bytes, an SPI queue: 800). Such a T is an
        // implicit-lifetime type: the byte array's lifetime starts the elements' as well.
        template<typename T, std::size_t Size>
        struct RawSlots {
            alignas(T) std::array<std::byte,
                                  sizeof(T) * Size> bytes;

            // through void*: a reinterpret_cast from std::byte* is gcc's -Wcast-align, though `bytes` is alignas(T)
            T* data() { return std::launder(static_cast<T*>(static_cast<void*>(bytes.data()))); }

            [[nodiscard]] T const* data() const {
                return std::launder(static_cast<T const*>(static_cast<void const*>(bytes.data())));
            }

            T& operator[](std::size_t i) { return data()[i]; }

            T const& operator[](std::size_t i) const { return data()[i]; }

            // byte for byte, as two std::array<T> of trivially copyable elements compare in a test
            friend bool operator==(RawSlots const&,
                                   RawSlots const&) = default;
        };

        template<typename T, std::size_t Size>
        using Slots = std::conditional_t<std::is_trivially_copyable_v<T>
                                           && std::is_trivially_destructible_v<T>,
                                         RawSlots<T, Size>,
                                         std::array<T, Size>>;

    }   // namespace Detail

    // A single producer / single consumer ring. It keeps one slot free to tell full from empty, so a
    // Queue<T, Size> holds Size - 1 elements (capacity()); Fifo<T, Capacity> below is the same queue
    // named by what it holds. The members follow std naming (popInto, max_size); BipBuffer and
    // Seqlock follow Kvasir's.
    template<typename TDataType,
             std::size_t Size,
             typename TOverflowPolicy = OverflowPolicyAssert,
             typename TSync           = DefaultSync>
    struct Queue {
        // indices 0 .. Size - 1: u8 up to Size 256, u16 up to 65536
        using IndexType = Detail::IndexFor<Size - 1>;
        static_assert(Size >= 1,
                      "a Queue needs at least one slot");
        // every stored index is in 0 .. Size - 1 (next, advanced, distance); comparisons with Size are
        // done in std::size_t
        static_assert(Size - 1 <= std::numeric_limits<IndexType>::max(),
                      "Size too big for the index type");

    private:
        friend struct QueueTestAccess;   // the host tests compare the raw state of two queues

        static constexpr auto          loadMemoryOrder{TSync::loadMemoryOrder};
        static constexpr auto          storeMemoryOrder{TSync::storeMemoryOrder};
        std::atomic<IndexType>         head_{};
        std::atomic<IndexType>         tail_{};
        Detail::Slots<TDataType, Size> data_{};

        static constexpr IndexType distance(IndexType head,
                                            IndexType tail) {
            auto const t = static_cast<std::size_t>(tail);
            auto const h = static_cast<std::size_t>(head);
            return static_cast<IndexType>(t >= h ? t - h : Size - h + t);
        }

        // An index is always below Size, so the wrap is one compare: `% Size` was a division on every
        // push and pop (a call into the RP2040's divider, software on the SAM D21: the Cortex-M0+ has no divide
        // instruction, DDI0484C Table 3-1, md l.813-930; the RP2040's SIO divider, its datasheet 2.3.1.5, md l.1519,
        // behind chip_rp2040's divider.S). A
        // power of two keeps its mask: clang does not find it from the compare.
        static constexpr IndexType next(IndexType in) {
            std::size_t const n = static_cast<std::size_t>(in) + 1;
            if constexpr(std::has_single_bit(Size)) {
                return static_cast<IndexType>(n & (Size - 1));
            } else {
                return static_cast<IndexType>(n == Size ? 0 : n);
            }
        }

        // contiguous trivially copyable ranges are copied in at most two runs instead of per element
        template<typename TRange>
        static constexpr bool copiesInRuns
          = std::ranges::contiguous_range<TRange> && std::is_trivially_copyable_v<TDataType>;

        static constexpr IndexType advanced(IndexType   index,
                                            std::size_t count) {
            std::size_t const end = std::size_t{index} + count;
            return IndexType(end >= Size ? end - Size : end);
        }

    public:
        void push(TDataType in) {
            auto const tail     = tail_.load(loadMemoryOrder);
            auto const head     = head_.load(loadMemoryOrder);
            auto const nextTail = next(tail);
            if(head != nextTail) {
                data_[tail] = in;
                TSync::fence();
                tail_.store(nextTail, storeMemoryOrder);   // commit
            } else {
                TOverflowPolicy{}();
            }
        }

        template<std::ranges::range TRange>
            requires std::same_as<std::ranges::range_value_t<TRange>,
                                  TDataType>
        void push(TRange const& range) {
            auto       tail = tail_.load(loadMemoryOrder);
            auto const head = head_.load(loadMemoryOrder);
            if(range.size() < Size - distance(head, tail)) {
                if constexpr(copiesInRuns<TRange const>) {
                    std::size_t const count = range.size();
                    std::size_t const first = std::min<std::size_t>(count, Size - tail);
                    std::copy_n(std::ranges::data(range), first, data_.data() + tail);
                    std::copy_n(std::ranges::data(range) + first, count - first, data_.data());
                    tail = advanced(tail, count);
                } else {
                    auto       begin = range.begin();
                    auto const end   = range.end();
                    while(begin != end) {
                        data_[tail] = *begin++;
                        tail        = next(tail);
                    }
                }
                TSync::fence();
                tail_.store(tail, storeMemoryOrder);   // commit
            } else {
                TOverflowPolicy{}();
            }
        }

        // Visit every queued element, oldest first, by reference (a bus that can cancel marks one cancelled in place).
        // Only with the producer and the consumer both excluded (the bus's interrupt masked): it reads head and tail
        // once and walks between them.
        template<typename F>
        void forEachQueued(F&& f) {
            auto const tail = tail_.load(loadMemoryOrder);
            auto const head = head_.load(loadMemoryOrder);
            TSync::acquire();
            for(auto i = head; i != tail; i = next(i)) { f(data_[i]); }
        }

        bool popInto(TDataType& out) {
            auto const tail = tail_.load(loadMemoryOrder);
            auto const head = head_.load(loadMemoryOrder);
            if(head == tail) { return false; }
            TSync::acquire();
            out = data_[head];
            TSync::fence();
            head_.store(next(head), storeMemoryOrder);   // commit
            return true;
        }

        template<std::ranges::range TRange>
            requires std::same_as<std::ranges::range_value_t<TRange>,
                                  TDataType>
        bool popInto(TRange& range) {
            auto const tail  = tail_.load(loadMemoryOrder);
            auto       head  = head_.load(loadMemoryOrder);
            auto const lsize = distance(head, tail);
            if(lsize < range.size()) { return false; }
            TSync::acquire();
            if constexpr(copiesInRuns<TRange>) {
                std::size_t const count = range.size();
                std::size_t const first = std::min<std::size_t>(count, Size - head);
                std::copy_n(data_.data() + head, first, std::ranges::data(range));
                std::copy_n(data_.data(), count - first, std::ranges::data(range) + first);
                head = advanced(head, count);
            } else {
                auto       begin = range.begin();
                auto const end   = range.end();
                while(begin != end) {
                    *begin++ = data_[head];
                    head     = next(head);
                }
            }
            TSync::fence();
            head_.store(head, storeMemoryOrder);   // commit
            return true;
        }

        void pop() {
            auto const tail = tail_.load(loadMemoryOrder);
            auto const head = head_.load(loadMemoryOrder);
            if(head == tail) { return; }
            TSync::fence();   // a front() read of the slot before the producer may reuse it
            head_.store(next(head), storeMemoryOrder);   // commit
        }

        // The oldest element. Precondition: !empty(). On an empty queue it calls the overflow policy
        // and, if that returns (OverflowPolicyIgnore, a counting policy), hands out a stale slot -
        // check empty() first, or use peek().
        [[nodiscard]] TDataType const& front() const {
            auto const head = head_.load(loadMemoryOrder);
            if(head == tail_.load(loadMemoryOrder)) { TOverflowPolicy{}(); }
            TSync::acquire();
            TDataType const& ret = data_[head];
            return ret;
        }

        // The oldest element, or nullptr when the queue is empty: front() without its precondition.
        // The pointer stays valid until the consumer's next pop.
        [[nodiscard]] TDataType const* peek() const {
            auto const head = head_.load(loadMemoryOrder);
            if(head == tail_.load(loadMemoryOrder)) { return nullptr; }
            TSync::acquire();
            return &data_[head];
        }

        [[nodiscard]] std::size_t size() const {
            return distance(head_.load(loadMemoryOrder), tail_.load(loadMemoryOrder));
        }

        [[nodiscard]] bool empty() const { return size() == 0; }

        [[nodiscard]] constexpr std::size_t max_size() const { return Size - 1; }

        // how many elements fit at once: Size - 1 (one slot stays free)
        static constexpr std::size_t capacity() { return Size - 1; }

        // Drop everything. Not concurrency-safe: for a producer or consumer that is provably
        // stopped (a SecondaryCore's primaryPrepare()), never while both are live.
        void clear() {
            head_.store(0, storeMemoryOrder);
            tail_.store(0, storeMemoryOrder);
        }
    };

    // A Queue named by how many elements it holds: Fifo<T, 8> takes 8 (a Queue<T, 9>), so a user
    // does not write the free slot's "+ 1" itself.
    template<typename TDataType,
             std::size_t Capacity,
             typename TOverflowPolicy = OverflowPolicyAssert,
             typename TSync           = DefaultSync>
        requires(Capacity >= 1)   // a Fifo that holds nothing
    using Fifo = Queue<TDataType, Capacity + 1, TOverflowPolicy, TSync>;
}}   // namespace Kvasir::Atomic
