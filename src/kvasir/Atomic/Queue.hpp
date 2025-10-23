#pragma once

#include "kvasir/Mpl/Utility.hpp"

#include <atomic>
#include <cassert>

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

    template<typename TDataType, std::size_t Size, typename TOverflowPolicy = OverFlowPolicyAssert>
    struct Queue {
        using IndexType = Detail::GetIndexTypeT<Size>;
        static_assert(std::numeric_limits<IndexType>::max() > Size,
                      "Size to big");
        static constexpr auto       load_memory_order{std::memory_order_relaxed};
        static constexpr auto       store_memory_order{std::memory_order_relaxed};
        static constexpr auto       fence_memory_order{std::memory_order_release};
        std::atomic<IndexType>      head_{};
        std::atomic<IndexType>      tail_{};
        std::array<TDataType, Size> data_{};

        static constexpr IndexType distance(IndexType head,
                                            IndexType tail) {
            auto d = int(unsigned(tail) - unsigned(head));
            if(d < 0) { d += Size; }
            return IndexType(d);
        }

        static constexpr IndexType next(IndexType in) { return (in + 1) % Size; }

        void push(TDataType in) {
            auto const tail     = tail_.load(load_memory_order);
            auto const head     = head_.load(load_memory_order);
            auto       nextTail = next(tail);
            if(head != nextTail) {
                data_[tail] = in;   //NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
                std::atomic_signal_fence(fence_memory_order);
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
                auto       begin = range.begin();
                auto const end   = range.end();
                while(begin != end) {
                    data_[tail]
                      = *begin++;   //NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
                    tail = next(tail);
                }
                std::atomic_signal_fence(fence_memory_order);
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
            std::atomic_signal_fence(fence_memory_order);
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
            auto       begin = range.begin();
            auto const end   = range.end();
            while(begin != end) {
                *begin++
                  = data_[head];   //NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
                head = next(head);
            }
            std::atomic_signal_fence(fence_memory_order);
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

        void clear() {
            head_.store(0, store_memory_order);
            tail_.store(0, store_memory_order);
        }
    };
}}   // namespace Kvasir::Atomic
