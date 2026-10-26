#pragma once

#include <algorithm>   // rotate, move, equal, lexicographical_compare_three_way
#include <array>
#include <bit>   // bit_cast
#include <cassert>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>   // memset, memcpy
#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory>   // destroy, addressof
#include <type_traits>
#include <utility>

namespace Kvasir {
namespace sv_detail {
    template<typename T>
    concept Trivial
      = std::is_trivially_copyable_v<T> && std::is_trivially_default_constructible_v<T>;

    // the trait mix the erase/resize members always asked for; weaker than std::movable on purpose
    template<typename T>
    concept Movable = std::is_object_v<T> && std::is_assignable_v<T&, T>
                   && std::is_move_constructible_v<T> && std::is_swappable_v<T&>;

    /// Smallest fixed-width unsigned integer type that can represent values in the range [0, N].
    template<std::size_t N>
    using smallest_size_t = std::conditional_t<
      (N < std::numeric_limits<std::uint8_t>::max()),
      std::uint8_t,
      std::conditional_t<
        (N < std::numeric_limits<std::uint16_t>::max()),
        std::uint16_t,
        std::conditional_t<(N < std::numeric_limits<std::uint32_t>::max()),
                           std::uint32_t,
                           std::conditional_t<(N < std::numeric_limits<std::uint64_t>::max()),
                                              std::uint64_t,
                                              std::size_t>>>>;

    /// Index a range, asserting the bound.
    template<typename Rng,
             typename Index>
    constexpr decltype(auto) index(Rng&&   rng,
                                   Index&& i) noexcept {
        assert(static_cast<std::ptrdiff_t>(i) < (std::end(rng) - std::begin(rng)));
        return std::begin(std::forward<Rng>(rng))[static_cast<std::ptrdiff_t>(i)];
    }

    /// Types implementing the `StaticVector`'s storage
    namespace storage {
        /// Storage for trivial types.
        template<typename T, std::size_t Capacity>
        struct Plain {
            static_assert(Trivial<T>,
                          "storage::Plain<T, C> requires Trivial<T>");
            static_assert(Capacity != 0,
                          "Capacity must be greater than zero!");

            using size_type       = smallest_size_t<Capacity>;
            using value_type      = T;
            using difference_type = std::ptrdiff_t;
            using pointer         = T*;
            using const_pointer   = T const*;

        private:
            // value-initialised: that is what makes the storage usable in a constant expression
            // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) // slots past size(): unread
            alignas(alignof(T)) std::array<T,
                                           Capacity> data_{};
            size_type size_ = 0;

        public:
            [[nodiscard]] constexpr const_pointer data() const noexcept { return data_.data(); }

            constexpr pointer data() noexcept { return data_.data(); }

            [[nodiscard]] constexpr size_type size() const noexcept { return size_; }

            static constexpr size_type capacity() noexcept { return Capacity; }

            [[nodiscard]] constexpr bool empty() const noexcept { return size() == 0; }

            [[nodiscard]] constexpr bool full() const noexcept { return size() == Capacity; }

            /// Contract: the storage is not full. By assignment, not placement new: constexpr.
            template<typename... Args>
                requires std::is_constructible_v<T,
                                                 Args...>
            constexpr void emplace_back(Args&&... args) noexcept {
                assert(!full() && "tried to emplace_back on full storage!");
                index(data_, size()) = T(std::forward<Args>(args)...);
                unsafeSetSize(size() + 1);
            }

            /// Contract: the storage is not empty.
            constexpr void pop_back() noexcept {
                assert(!empty() && "tried to pop_back from empty storage!");
                // the contract under NDEBUG too: without it the analyzer pops below data_ on the paths where a
                // caller's earlier check failed (clang-analyzer-security.ArrayBound in an NDEBUG test tree)
                [[assume(size_ != 0)]];
                unsafeSetSize(size() - 1);
            }

            /// (unsafe) Changes the size; no element is constructed or destroyed.
            constexpr void unsafeSetSize(std::size_t newSize) noexcept {
                assert(newSize <= Capacity && "new_size out-of-bounds [0, Capacity]");
                size_ = size_type(newSize);
            }

            template<typename It>
            constexpr void unsafeDestroy(It,
                                         It) noexcept {}

            static constexpr void unsafeDestroyAll() noexcept {}

            constexpr Plain() noexcept = default;

            /// Contract: `il.size() <= capacity()`.
            template<std::convertible_to<T> U>
            constexpr Plain(std::initializer_list<U> il) noexcept : data_(fromList_(il)) {
                unsafeSetSize(static_cast<size_type>(il.size()));
            }

        private:
            template<typename U>
            static constexpr std::array<T,
                                        Capacity>
            fromList_(std::initializer_list<U>& il) noexcept {
                assert(il.size() <= capacity()
                       && "trying to construct storage from an initializer_list whose size exceeds the storage capacity");
                std::array<T, Capacity> d_{};
                for(std::size_t i = 0, e = il.size(); i < e; ++i) {
                    index(d_, i) = static_cast<T>(index(il, i));
                }
                return d_;
            }
        };

        /// Storage for non-trivial elements.
        template<typename T, std::size_t Capacity>
        struct Managed {
            static_assert(!Trivial<T>,
                          "use storage::Plain for Trivial<T> elements");
            static_assert(Capacity != 0,
                          "Capacity must be greater than zero!");

            using size_type       = smallest_size_t<Capacity>;
            using value_type      = T;
            using difference_type = std::ptrdiff_t;
            using pointer         = T*;
            using const_pointer   = T const*;

        private:
            size_type size_ = 0;

            // Raw storage, deliberately not an array of T: lifetimes are managed by hand
            // here, so live T objects would be constructed and destroyed twice per slot.
            alignas(alignof(T)) std::byte data_[sizeof(T) * Capacity];

        public:
            // Through void*: data_ is alignas(T), which gcc's -Wcast-align (strict on ARM) does not see.
            [[nodiscard]] const_pointer data() const noexcept {
                return static_cast<const_pointer>(static_cast<void const*>(data_));
            }

            pointer data() noexcept { return static_cast<pointer>(static_cast<void*>(data_)); }

            [[nodiscard]] const_pointer end() const noexcept { return data() + size(); }

            pointer end() noexcept { return data() + size(); }

            [[nodiscard]] constexpr size_type size() const noexcept { return size_; }

            static constexpr size_type capacity() noexcept { return Capacity; }

            [[nodiscard]] constexpr bool empty() const noexcept { return size() == 0; }

            [[nodiscard]] constexpr bool full() const noexcept { return size() == Capacity; }

            /// Contract: the storage is not full.
            template<typename... Args>
                requires std::is_constructible_v<T,
                                                 Args...>
            void emplace_back(Args&&... args) noexcept(std::is_nothrow_constructible_v<T,
                                                                                       Args...>) {
                assert(!full() && "tried to emplace_back on full storage");
                new(end()) T(std::forward<Args>(args)...);
                unsafeSetSize(size() + 1);
            }

            /// Contract: the storage is not empty.
            void pop_back() noexcept {
                assert(!empty() && "tried to pop_back from empty storage!");
                // the contract under NDEBUG too: without it the analyzer pops below data_ on the paths where a
                // caller's earlier check failed (clang-analyzer-security.ArrayBound in an NDEBUG test tree)
                [[assume(size_ != 0)]];
                auto ptr = end() - 1;
                ptr->~T();
                unsafeSetSize(size() - 1);
            }

            /// (unsafe) Changes the size; no element is constructed or destroyed.
            constexpr void unsafeSetSize(std::size_t newSize) noexcept {
                assert(newSize <= Capacity && "new_size out-of-bounds [0, Capacity)");
                size_ = size_type(newSize);
            }

            /// (unsafe) Destroys [first, last); the size is not changed.
            void unsafeDestroy(pointer first,
                               pointer last) noexcept {
                assert(first >= data() && first <= end() && "first is out-of-bounds");
                assert(last >= data() && last <= end() && "last is out-of-bounds");
                // std::destroy, not `for(; first != last; ++first) first->~T();`: clang 23's
                // -Wlifetime-safety-invalidation takes the destructor call for an invalidation
                // of `first` and the next `++` / `!=` for a use after it.
                std::destroy(first, last);
            }

            void unsafeDestroyAll() noexcept { unsafeDestroy(data(), end()); }

            // the storage is raw, so the special members cannot be defaulted: each has to
            // construct or destroy exactly [0, size()). The default constructor is user
            // provided so that `non_trivial const x;` stays well-formed.
            // NOLINTNEXTLINE(modernize-use-equals-default) // user provided on purpose: see above
            constexpr Managed() noexcept {}

            /// A T that is only "non-trivial" because of its assignment operators -- std::pair,
            /// what every StaticMap stores -- still copies as bytes: an implicit-lifetime type,
            /// trivially copy/move constructible and trivially destructible. Copying such a map
            /// element by element through emplace_back was a loop of calls per entry.
            static constexpr bool bytewiseCopy
              = std::is_trivially_copy_constructible_v<T> && std::is_trivially_destructible_v<T>;
            static constexpr bool bytewiseMove
              = std::is_trivially_move_constructible_v<T> && std::is_trivially_destructible_v<T>;

            void unsafeCopyBytes(Managed const& other) noexcept {
                std::memcpy(data_, other.data_, sizeof(T) * other.size());
                unsafeSetSize(other.size());
            }

            Managed(Managed const& other) noexcept(std::is_nothrow_copy_constructible_v<T>) {
                constructFrom_<false>(other);
            }

            Managed(Managed&& other) noexcept(std::is_nothrow_move_constructible_v<T>) {
                constructFrom_<true>(other);
            }

            Managed&
            operator=(Managed const& other) noexcept(std::is_nothrow_copy_constructible_v<T>) {
                if(this != std::addressof(other)) {
                    unsafeDestroyAll();
                    unsafeSetSize(0);
                    constructFrom_<false>(other);
                }
                return *this;
            }

            Managed& operator=(Managed&& other) noexcept(std::is_nothrow_move_constructible_v<T>) {
                if(this != std::addressof(other)) {
                    unsafeDestroyAll();
                    unsafeSetSize(0);
                    constructFrom_<true>(other);
                }
                return *this;
            }

            // Elements with no destructor need none run: then the storage is trivially
            // destructible, and so is every variant, optional and expected that holds it.
            ~Managed()
                requires std::is_trivially_destructible_v<T>
            = default;

            ~Managed()
                requires(!std::is_trivially_destructible_v<T>)
            {
                unsafeDestroyAll();
            }

            /// Contract: `il.size() <= capacity()`.
            template<std::convertible_to<T> U>
            Managed(std::initializer_list<U> il) {
                assert(il.size() <= capacity()
                       && "trying to construct storage from an initializer_list whose size exceeds the storage capacity");
                for(auto const& v : il) { emplace_back(v); }
            }

        private:
            /// Fills an empty storage from \p other: as bytes where T allows it, else element by element.
            template<bool Move,
                     typename Other>
            void constructFrom_(Other& other) {
                if constexpr(Move ? bytewiseMove : bytewiseCopy) {
                    unsafeCopyBytes(other);
                } else {
                    for(size_type i = 0; i != other.size(); ++i) {
                        if constexpr(Move) {
                            emplace_back(std::move(other.data()[i]));
                        } else {
                            emplace_back(other.data()[i]);
                        }
                    }
                }
            }
        };

        /// Selects the vector storage.
        template<typename T, std::size_t Capacity>
        using _t = std::conditional_t<Trivial<T>, Plain<T, Capacity>, Managed<T, Capacity>>;
    }   // namespace storage
}   // namespace sv_detail

/// Dynamically-resizable fixed-capacity vector, shaped like std::inplace_vector.
///
/// It declares no copy/move member of its own: the storage's implicit ones are used, so a
/// StaticVector of a trivial T is trivially copyable (util_static_vector_tests.cpp asserts it).
/// Do not add a user-declared special member here; extend the storage instead.
template<typename T, std::size_t Capacity>
struct StaticVector : private sv_detail::storage::_t<T, Capacity> {
private:
    static_assert(!std::is_const_v<T>,
                  "StaticVector<const T, N> is not supported");
    static_assert(std::is_nothrow_destructible_v<T>,
                  "T must be nothrow destructible");
    using base_t = sv_detail::storage::_t<T, Capacity>;

    using base_t::unsafeDestroy;
    using base_t::unsafeDestroyAll;
    using base_t::unsafeSetSize;

public:
    using value_type             = T;
    using difference_type        = std::ptrdiff_t;
    using reference              = value_type&;
    using const_reference        = value_type const&;
    using pointer                = T*;
    using const_pointer          = T const*;
    using iterator               = T*;
    using const_iterator         = T const*;
    using size_type              = std::size_t;
    using reverse_iterator       = std::reverse_iterator<iterator>;
    using const_reverse_iterator = std::reverse_iterator<const_iterator>;

    // size / capacity
    using base_t::empty;
    using base_t::full;

    [[nodiscard]] constexpr size_type size() const noexcept { return base_t::size(); }

    static constexpr size_type capacity() noexcept { return base_t::capacity(); }

    static constexpr size_type max_size() noexcept { return capacity(); }

    // data access, iterators
    using base_t::data;

    constexpr iterator begin() noexcept { return data(); }

    [[nodiscard]] constexpr const_iterator begin() const noexcept { return data(); }

    constexpr iterator end() noexcept { return data() + size(); }

    [[nodiscard]] constexpr const_iterator end() const noexcept { return data() + size(); }

    [[nodiscard]] constexpr const_iterator cbegin() const noexcept { return begin(); }

    [[nodiscard]] constexpr const_iterator cend() const noexcept { return end(); }

    constexpr reverse_iterator rbegin() noexcept { return reverse_iterator(end()); }

    [[nodiscard]] constexpr const_reverse_iterator rbegin() const noexcept {
        return const_reverse_iterator(end());
    }

    constexpr reverse_iterator rend() noexcept { return reverse_iterator(begin()); }

    [[nodiscard]] constexpr const_reverse_iterator rend() const noexcept {
        return const_reverse_iterator(begin());
    }

    // element access (bounds asserted)
    constexpr reference operator[](size_type pos) noexcept { return sv_detail::index(*this, pos); }

    constexpr const_reference operator[](size_type pos) const noexcept {
        return sv_detail::index(*this, pos);
    }

    constexpr reference front() noexcept { return sv_detail::index(*this, 0); }

    [[nodiscard]] constexpr const_reference front() const noexcept {
        return sv_detail::index(*this, 0);
    }

    constexpr reference back() noexcept {
        assert(!empty() && "calling back on an empty vector");
        return sv_detail::index(*this, size() - 1);
    }

    [[nodiscard]] constexpr const_reference back() const noexcept {
        assert(!empty() && "calling back on an empty vector");
        return sv_detail::index(*this, size() - 1);
    }

    // modifiers
    using base_t::emplace_back;
    using base_t::pop_back;

    constexpr void clear() noexcept {
        unsafeDestroyAll();
        unsafeSetSize(0);
    }

    // the storage's emplace_back asserts !full()
    template<typename U>
        requires std::is_constructible_v<T,
                                         U>
    constexpr void push_back(U&& value) {
        emplace_back(std::forward<U>(value));
    }

    /// std::inplace_vector's try_ spellings: nullptr when full, no assert.
    template<typename... Args>
        requires std::is_constructible_v<T,
                                         Args...>
    constexpr pointer try_emplace_back(Args&&... args) {
        if(full()) { return nullptr; }
        emplace_back(std::forward<Args>(args)...);
        return std::addressof(back());
    }

    constexpr pointer try_push_back(T const& value)
        requires std::is_copy_constructible_v<T>
    {
        return try_emplace_back(value);
    }

    constexpr pointer try_push_back(T&& value)
        requires std::is_move_constructible_v<T>
    {
        return try_emplace_back(std::move(value));
    }

    /// std::inplace_vector's unchecked_ spellings: no full() check of the vector's own; the
    /// storage's `!full()` assert still runs (every precondition stays checked).
    template<typename... Args>
        requires std::is_constructible_v<T,
                                         Args...>
    constexpr reference unchecked_emplace_back(Args&&... args) {
        emplace_back(std::forward<Args>(args)...);
        return back();
    }

    constexpr reference unchecked_push_back(T const& value)
        requires std::is_copy_constructible_v<T>
    {
        return unchecked_emplace_back(value);
    }

    constexpr reference unchecked_push_back(T&& value)
        requires std::is_move_constructible_v<T>
    {
        return unchecked_emplace_back(std::move(value));
    }

    template<typename... Args>
        requires std::is_constructible_v<T,
                                         Args...>
    constexpr iterator emplace(const_iterator position,
                               Args&&... args) {
        value_type a(std::forward<Args>(args)...);
        return appendThenRotate_<true>(position, &a, &a + 1);
    }

    constexpr iterator insert(const_iterator  position,
                              const_reference x)
        requires std::is_copy_constructible_v<T>
    {
        return insert(position, size_type{1}, x);
    }

    // x is moved from via [&x, &x + 1)
    constexpr iterator insert(const_iterator position,
                              value_type&&   x)
        requires std::is_move_constructible_v<T>
    {
        return appendThenRotate_<true>(position, &x, &x + 1);
    }

    constexpr iterator insert(const_iterator position,
                              size_type      n,
                              T const&       x)
        requires std::is_copy_constructible_v<T>
    {
        assertIteratorInRange_(position);
        assert(size() + n <= capacity() && "trying to insert beyond capacity!");
        auto const old = end();
        for(; n != 0; --n) { push_back(x); }
        return rotateToPosition_(position, old);
    }

    template<std::input_iterator It>
        requires std::is_constructible_v<T,
                                         std::iter_reference_t<It>>
    constexpr iterator insert(const_iterator position,
                              It             first,
                              It             last) {
        return appendThenRotate_<false>(position, first, last);
    }

    constexpr iterator insert(const_iterator           position,
                              std::initializer_list<T> il)
        requires std::is_copy_constructible_v<T>
    {
        return insert(position, il.begin(), il.end());
    }

    constexpr iterator erase(const_iterator position) noexcept
        requires sv_detail::Movable<T>
    {
        assertIteratorInRange_(position);
        return erase(position, position + 1);
    }

    constexpr iterator erase(const_iterator first,
                             const_iterator last) noexcept
        requires sv_detail::Movable<T>
    {
        assertIteratorInRange_(first);
        assertIteratorInRange_(last);
        assert(first <= last && "invalid iterator pair");
        iterator p = begin() + (first - begin());
        if(first != last) {
            unsafeDestroy(std::move(p + (last - first), end(), p), end());
            unsafeSetSize(size() - static_cast<size_type>(last - first));
        }
        return p;
    }

    // element by element: the common prefix swapped, the longer one's tail moved over (no temporary vector)
    constexpr void swap(StaticVector& other) noexcept(std::is_nothrow_swappable_v<T>)
        requires sv_detail::Movable<T>
    {
        StaticVector& shorter = size() <= other.size() ? *this : other;
        StaticVector& longer  = size() <= other.size() ? other : *this;
        auto const    common  = static_cast<difference_type>(shorter.size());
        std::swap_ranges(shorter.begin(), shorter.end(), longer.begin());
        for(auto it = longer.begin() + common; it != longer.end(); ++it) {
            shorter.emplace_back(std::move(*it));
        }
        longer.truncate_(static_cast<size_type>(common));
    }

    /// Resizes to \p sz elements; appended ones are copies of \p value.
    constexpr void resize(size_type sz,
                          T const&  value)
        requires std::is_copy_constructible_v<T>
    {
        if(sz == size()) { return; }
        if constexpr(sv_detail::Trivial<T>) {
            resizeTrivial_(sz, value);
        } else if(sz > size()) {
            assert(sz <= capacity()
                   && "StaticVector cannot be resized to a size greater than capacity");
            insert(end(), sz - size(), value);
        } else {
            truncate_(sz);
        }
    }

    /// Resizes to \p sz elements; appended ones are value-initialised.
    constexpr void resize(size_type sz)
        requires sv_detail::Movable<T>
    {
        if(sz == size()) { return; }
        if constexpr(sv_detail::Trivial<T>) {
            resizeTrivial_(sz, T{});
        } else if(sz > size()) {
            emplaceN_(sz);
        } else {
            truncate_(sz);
        }
    }

    /// std::string's resize_and_overwrite for a trivial T: \p op(data(), n) may write all \p n
    /// elements and returns how many to keep, which becomes the size. The elements it had are
    /// still there when \p op runs; nothing is filled, so what \p op keeps and did not write is
    /// whatever the storage held.
    ///
    /// Contract: `n <= capacity()`, and \p op returns at most \p n.
    template<typename Op>
        requires sv_detail::Trivial<T>
    constexpr void resize_and_overwrite(size_type n,
                                        Op&&      op) {
        assert(n <= capacity() && "StaticVector cannot be resized to a size greater than capacity");
        auto const kept = static_cast<size_type>(std::forward<Op>(op)(data(), n));
        assert(kept <= n && "resize_and_overwrite: op kept more than it was given");
        unsafeSetSize(kept);
    }

    // construct / assign
    constexpr StaticVector() = default;

    /// \p n value-initialised elements.
    explicit constexpr StaticVector(size_type n)
        requires std::is_copy_constructible_v<T> || std::is_move_constructible_v<T>
    {
        emplaceN_(n);
    }

    /// \p n copies of \p value.
    constexpr StaticVector(size_type n,
                           T const&  value)
        requires std::is_copy_constructible_v<T>
    {
        assert(n <= capacity() && "size exceeds capacity");
        insert(begin(), n, value);
    }

    template<std::input_iterator It>
    constexpr StaticVector(It first,
                           It last) {
        insert(begin(), first, last);
    }

    template<std::convertible_to<T> U>
    constexpr StaticVector(std::initializer_list<U> il)   // the storage asserts the size
      : base_t(il) {}

    template<std::input_iterator It>
    constexpr void assign(It first,
                          It last) {
        assertFits_(first, last, 0);
        clear();
        insert(begin(), first, last);
    }

    constexpr void assign(size_type n,
                          T const&  u)
        requires std::is_copy_constructible_v<T>
    {
        assert(n <= capacity() && "size exceeds capacity");
        clear();
        insert(begin(), n, u);
    }

    constexpr void assign(std::initializer_list<T> il)
        requires std::is_copy_constructible_v<T>
    {
        assign(il.begin(), il.end());
    }

    // comparison: == and <=> as hidden friends; C++20 rewrites !=, <, <=, >, >=
    friend constexpr bool operator==(StaticVector const& a,
                                     StaticVector const& b) noexcept {
        return std::equal(a.begin(), a.end(), b.begin(), b.end());
    }

    /// Lexicographic; a T with only `<` compares through a weak_ordering synthesised from it, as
    /// std::vector does.
    friend constexpr auto operator<=>(StaticVector const& a,
                                      StaticVector const& b)
        requires requires(T const& x) { x < x; }
    {
        return std::lexicographical_compare_three_way(
          a.begin(),
          a.end(),
          b.begin(),
          b.end(),
          [](T const& x, T const& y) {
              if constexpr(std::three_way_comparable<T>) {
                  return x <=> y;
              } else {
                  if(x < y) { return std::weak_ordering::less; }
                  if(y < x) { return std::weak_ordering::greater; }
                  return std::weak_ordering::equivalent;
              }
          });
    }

private:
    template<typename It>
    constexpr void assertIteratorInRange_(It it [[maybe_unused]]) const noexcept {
        static_assert(std::is_pointer_v<It>);
        assert(begin() <= it && "iterator not in range");
        assert(it <= end() && "iterator not in range");
    }

    /// The one capacity check of a range insert; only a random-access range knows its length up front
    /// (an input range trips the storage's full() assert instead).
    template<typename It>
    constexpr void assertFits_(It        first [[maybe_unused]],
                               It        last [[maybe_unused]],
                               size_type kept [[maybe_unused]]) const noexcept {
        if constexpr(std::random_access_iterator<It>) {
            assert(first <= last && "invalid iterator pair");
            assert(kept + static_cast<size_type>(last - first) <= capacity()
                   && "trying to insert beyond capacity!");
        }
    }

    /// Moves [old, end()) to \p position (insert at the end, then rotate).
    constexpr iterator rotateToPosition_(const_iterator position,
                                         iterator       old) {
        auto const at = begin() + (position - begin());
        std::rotate(at, old, end());
        return at;
    }

    template<bool Move,
             typename It>
    constexpr iterator appendThenRotate_(const_iterator position,
                                         It             first,
                                         It             last) {
        assertIteratorInRange_(position);
        assertFits_(first, last, size());
        auto const old = end();
        for(; first != last; ++first) {
            if constexpr(Move) {
                emplace_back(std::move(*first));
            } else {
                emplace_back(*first);
            }
        }
        return rotateToPosition_(position, old);
    }

    /// A trivial T needs no per-element construction or destruction, so a resize is a fill of
    /// the new tail and one size update. Appending element by element cost ~100 cycles per
    /// element on a Cortex-M33 at -Oz -- and aglio grows its byte buffer by a resize per
    /// serialized field, which made packing a 700-byte response take most of a millisecond.
    constexpr void resizeTrivial_(size_type sz,
                                  T const&  value) noexcept {
        assert(sz <= capacity()
               && "StaticVector cannot be resized to a size greater than capacity");
        if(sz > size()) {
            auto const first = end();
            auto const added = sz - size();
            unsafeSetSize(sz);
            if constexpr(sizeof(T) == 1) {
                // -Oz keeps the loop below a loop, one byte per iteration.
                if(!std::is_constant_evaluated()) {
                    std::memset(first, std::bit_cast<unsigned char>(value), added);
                    return;
                }
            }
            for(auto it = first; it != end(); ++it) { *it = value; }
        } else {
            unsafeSetSize(sz);
        }
    }

    /// The elements past the first \p sz destroyed, the size \p sz (`sz <= size()`): erase() of a tail without its
    /// move and its iterator pair. gcc -O3 warned (-Wmaybe-uninitialized) for the pair passed to an out-of-line erase(),
    /// pointers to const into storage it never saw written.
    constexpr void truncate_(size_type sz) noexcept {
        unsafeDestroy(begin() + static_cast<difference_type>(sz), end());
        unsafeSetSize(sz);
    }

    constexpr void emplaceN_(size_type n) {
        assert(n <= capacity() && "StaticVector cannot be resized to a size greater than capacity");
        while(n != size()) { emplace_back(T{}); }
    }
};

}   // namespace Kvasir
