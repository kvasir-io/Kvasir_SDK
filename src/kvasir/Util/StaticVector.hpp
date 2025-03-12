#pragma once

#include <array>
#include <cassert>
#include <cstddef>       // for size_t
#include <cstdint>       // for fixed-width integer types
#include <cstdio>        // for assertion diagnostics
#include <functional>    // for less and equal_to
#include <iterator>      // for reverse_iterator and iterator traits
#include <limits>        // for numeric_limits
#include <stdexcept>     // for length_error
#include <type_traits>   // for aligned_storage and all meta-functions

#define SV_CONCEPT_PP_CAT_(X, Y) X##Y
#define SV_CONCEPT_PP_CAT(X, Y)  SV_CONCEPT_PP_CAT_(X, Y)

/// Requires-clause emulation with SFINAE (for templates)
#define SV_REQUIRES_(...)                                                                 \
    int SV_CONCEPT_PP_CAT(_concept_requires_, __LINE__)                                   \
      = 42,                                                                               \
      typename ::std::enable_if < (SV_CONCEPT_PP_CAT(_concept_requires_, __LINE__) == 43) \
        || (__VA_ARGS__),                                                                 \
      int > ::type = 0 /**/

/// Requires-clause emulation with SFINAE (for "non-templates")
#define SV_REQUIRES(...)                                                          \
    template<                                                                     \
      int SV_CONCEPT_PP_CAT(_concept_requires_, __LINE__) = 42,                   \
      typename ::std::enable_if<                                                  \
        (SV_CONCEPT_PP_CAT(_concept_requires_, __LINE__) == 43) || (__VA_ARGS__), \
        int>::type                                                                \
      = 0> /**/

namespace Kvasir {
// Private utilites
namespace sv_detail {
    /// \name Utilities
    ///@{

    template<bool v>
    using bool_ = std::integral_constant<bool, v>;

    /// \name Concepts (poor-man emulation using type traits)
    ///@{
    template<typename T, typename... Args>
    static constexpr bool Constructible = std::is_constructible_v<T, Args...>;

    template<typename T>
    static constexpr bool CopyConstructible = std::is_copy_constructible_v<T>;

    template<typename T>
    static constexpr bool MoveConstructible = std::is_move_constructible_v<T>;

    template<typename T, typename U>
    static constexpr bool Assignable = std::is_assignable_v<T, U>;

    template<typename T>
    static constexpr bool Movable
      = std::is_object_v<T> && Assignable<T&, T> && MoveConstructible<T> && std::is_swappable_v<T&>;

    template<typename From, typename To>
    static constexpr bool Convertible = std::is_convertible_v<From, To>;

    template<typename T>
    static constexpr bool Trivial = std::is_trivial_v<T>;

    template<typename T>
    static constexpr bool Const = std::is_const_v<T>;

    template<typename T>
    static constexpr bool Pointer = std::is_pointer_v<T>;
    ///@}  // Concepts

    template<typename Rng>
    using range_iterator_t = decltype(std::begin(std::declval<Rng>()));

    template<typename T>
    using iterator_reference_t = typename std::iterator_traits<T>::reference;

    template<typename T>
    using iterator_category_t = typename std::iterator_traits<T>::iterator_category;

    template<typename T, typename Cat, typename = void>
    struct Iterator_ : std::false_type {};

    template<typename T, typename Cat>
    struct Iterator_<T, Cat, std::void_t<iterator_category_t<T>>>
      : bool_<Convertible<iterator_category_t<T>, Cat>> {};

    /// \name Concepts (poor-man emulation using type traits)
    ///@{
    template<typename T>
    static constexpr bool InputIterator = Iterator_<T, std::input_iterator_tag>{};

    template<typename T>
    static constexpr bool ForwardIterator = Iterator_<T, std::forward_iterator_tag>{};

    template<typename T>
    static constexpr bool OutputIterator
      = Iterator_<T, std::output_iterator_tag>{} || ForwardIterator<T>;

    template<typename T>
    static constexpr bool BidirectionalIterator = Iterator_<T, std::bidirectional_iterator_tag>{};

    template<typename T>
    static constexpr bool RandomAccessIterator = Iterator_<T, std::random_access_iterator_tag>{};

    template<typename T>
    static constexpr bool RandomAccessRange = RandomAccessIterator<range_iterator_t<T>>;
    ///@}  // Concepts

    /// Smallest fixed-width unsigned integer type that can represent
    /// values in the range [0, N].
    template<size_t N>
    using smallest_size_t = std::conditional_t<
      (N < std::numeric_limits<uint8_t>::max()),
      uint8_t,
      std::conditional_t<
        (N < std::numeric_limits<uint16_t>::max()),
        uint16_t,
        std::conditional_t<
          (N < std::numeric_limits<uint32_t>::max()),
          uint32_t,
          std::conditional_t<(N < std::numeric_limits<uint64_t>::max()), uint64_t, size_t>>>>;

    /// Index a range doing bound checks in debug builds
    template<typename Rng, typename Index, SV_REQUIRES_(RandomAccessRange<Rng>)>
    constexpr decltype(auto) index(Rng&& rng, Index&& i) noexcept {
        assert(static_cast<ptrdiff_t>(i) < (std::end(rng) - std::begin(rng)));
        return std::begin(std::forward<Rng>(rng))[std::forward<Index>(i)];
    }

    /// \name Workarounds
    ///@{

    // WORKAROUND: std::rotate is not constexpr
    template<typename ForwardIt, SV_REQUIRES_(ForwardIterator<ForwardIt>)>
    constexpr void slow_rotate(ForwardIt first, ForwardIt n_first, ForwardIt last) {
        ForwardIt next = n_first;
        while(first != next) {
            using std::swap;
            swap(*(first++), *(next++));
            if(next == last) {
                next = n_first;
            } else if(first == n_first) {
                n_first = next;
            }
        }
    }

    // WORKAROUND: std::move is not constexpr
    template<
      typename InputIt,
      typename OutputIt,
      SV_REQUIRES_(InputIterator<InputIt>&& OutputIterator<OutputIt>)>
    constexpr OutputIt move(InputIt b, InputIt e, OutputIt to) {
        for(; b != e; ++b, (void)++to) {
            *to = ::std::move(*b);
        }
        return to;
    }

    // WORKAROUND: std::equal is not constexpr
    template<
      class BinaryPredicate,
      class InputIterator1,
      class InputIterator2,
      SV_REQUIRES_(InputIterator<InputIterator1>&& InputIterator<InputIterator2>)>
    constexpr bool cmp(
      InputIterator1  first1,
      InputIterator1  last1,
      InputIterator2  first2,
      InputIterator2  last2,
      BinaryPredicate pred) {
        for(; first1 != last1 && first2 != last2; ++first1, (void)++first2) {
            if(!pred(*first1, *first2)) {
                return false;
            }
        }
        return first1 == last1 && first2 == last2;
    }

    ///@}  // Workarounds

    ///@} // Utilities

    /// Types implementing the `StaticVector`'s storage
    namespace storage {
        /// Storage for zero elements.
        template<typename T>
        struct zero_sized {
            using size_type       = uint8_t;
            using value_type      = T;
            using difference_type = ptrdiff_t;
            using pointer         = T*;
            using const_pointer   = T const*;

            /// Pointer to the data in the storage.
            static constexpr pointer data() noexcept { return nullptr; }
            /// Number of elements currently stored.
            static constexpr size_type size() noexcept { return 0; }
            /// Capacity of the storage.
            static constexpr size_type capacity() noexcept { return 0; }
            /// Is the storage empty?
            static constexpr bool empty() noexcept { return true; }
            /// Is the storage full?
            static constexpr bool full() noexcept { return true; }

            /// Constructs a new element at the end of the storage
            /// in-place.
            ///
            /// Increases size of the storage by one.
            /// Always fails for empty storage.
            template<typename... Args, SV_REQUIRES_(Constructible<T, Args...>)>
            static constexpr void emplace_back(Args&&...) noexcept {
                assert(false && "tried to emplace_back on empty storage");
            }
            /// Removes the last element of the storage.
            /// Always fails for empty storage.
            static constexpr void pop_back() noexcept {
                assert(false && "tried to pop_back on empty storage");
            }
            /// Changes the size of the storage without adding or
            /// removing elements (unsafe).
            ///
            /// The size of an empty storage can only be changed to 0.
            static constexpr void unsafe_set_size(size_t new_size) noexcept {
                assert(new_size == 0 && "tried to change size of empty storage to non-zero value");
            }

            /// Destroys all elements of the storage in range [begin,
            /// end) without changings its size (unsafe).
            ///
            /// Nothing to destroy since the storage is empty.
            template<typename InputIt, SV_REQUIRES_(InputIterator<InputIt>)>
            static constexpr void unsafe_destroy(InputIt /* begin */, InputIt /* end */) noexcept {}

            /// Destroys all elements of the storage without changing
            /// its size (unsafe).
            ///
            /// Nothing to destroy since the storage is empty.
            static constexpr void unsafe_destroy_all() noexcept {}

            constexpr zero_sized()                                 = default;
            constexpr zero_sized(zero_sized const&)                = default;
            constexpr zero_sized& operator=(zero_sized const&)     = default;
            constexpr zero_sized(zero_sized&&) noexcept            = default;
            constexpr zero_sized& operator=(zero_sized&&) noexcept = default;
            ~zero_sized() noexcept                                 = default;

            /// Constructs an empty storage from an initializer list of
            /// zero elements.
            template<typename U, SV_REQUIRES_(Convertible<U, T>)>
            constexpr zero_sized(std::initializer_list<U> il) noexcept {
                assert(
                  il.size() == 0
                  && "tried to construct storage::empty from a non-empty initializer list");
            }
        };

        /// Storage for trivial types.
        template<typename T, size_t Capacity>
        struct trivial {
            static_assert(Trivial<T>, "storage::trivial<T, C> requires Trivial<T>");
            static_assert(
              Capacity != size_t{0},
              "Capacity must be greater than zero (use storage::zero_sized instead)");

            using size_type       = smallest_size_t<Capacity>;
            using value_type      = T;
            using difference_type = ptrdiff_t;
            using pointer         = T*;
            using const_pointer   = T const*;

        private:
            // If the value_type is const, make a const array of
            // non-const elements:
            using data_t = std::conditional_t<
              !Const<T>,
              std::array<T, Capacity>,
              std::array<std::remove_const_t<T>, Capacity> const>;
            alignas(alignof(T)) data_t data_{};

            /// Number of elements allocated in the storage:
            size_type size_ = 0;

        public:
            /// Direct access to the underlying storage.
            ///
            /// Complexity: O(1) in time and space.
            constexpr const_pointer data() const noexcept { return data_.data(); }

            /// Direct access to the underlying storage.
            ///
            /// Complexity: O(1) in time and space.
            constexpr pointer data() noexcept { return data_.data(); }

            /// Number of elements in the storage.
            ///
            /// Complexity: O(1) in time and space.
            constexpr size_type size() const noexcept { return size_; }

            /// Maximum number of elements that can be allocated in the
            /// storage.
            ///
            /// Complexity: O(1) in time and space.
            static constexpr size_type capacity() noexcept { return Capacity; }

            /// Is the storage empty?
            constexpr bool empty() const noexcept { return size() == size_type{0}; }

            /// Is the storage full?
            constexpr bool full() const noexcept { return size() == Capacity; }

            /// Constructs an element in-place at the end of the
            /// storage.
            ///
            /// Complexity: O(1) in time and space.
            /// Contract: the storage is not full.
            template<
              typename... Args,
              SV_REQUIRES_(Constructible<T, Args...>and Assignable<value_type&, T>)>
            constexpr void emplace_back(Args&&... args) noexcept {
                assert(!full() && "tried to emplace_back on full storage!");
                index(data_, size()) = T(std::forward<Args>(args)...);
                unsafe_set_size(size() + 1);
            }

            /// Remove the last element from the container.
            ///
            /// Complexity: O(1) in time and space.
            /// Contract: the storage is not empty.
            constexpr void pop_back() noexcept {
                assert(!empty() && "tried to pop_back from empty storage!");
                unsafe_set_size(size() - 1);
            }

            /// (unsafe) Changes the container size to \p new_size.
            ///
            /// Contract: `new_size <= capacity()`.
            /// \warning No elements are constructed or destroyed.
            constexpr void unsafe_set_size(size_t new_size) noexcept {
                assert(new_size <= Capacity && "new_size out-of-bounds [0, Capacity]");
                size_ = size_type(new_size);
            }

            /// (unsafe) Destroy elements in the range [begin, end).
            ///
            /// \warning: The size of the storage is not changed.
            template<typename InputIt, SV_REQUIRES_(InputIterator<InputIt>)>
            constexpr void unsafe_destroy(InputIt, InputIt) noexcept {}

            /// (unsafe) Destroys all elements of the storage.
            ///
            /// \warning: The size of the storage is not changed.
            static constexpr void unsafe_destroy_all() noexcept {}

            constexpr trivial() noexcept                          = default;
            constexpr trivial(trivial const&) noexcept            = default;
            constexpr trivial& operator=(trivial const&) noexcept = default;
            constexpr trivial(trivial&&) noexcept                 = default;
            constexpr trivial& operator=(trivial&&) noexcept      = default;
            ~trivial()                                            = default;

        private:
            template<typename U, SV_REQUIRES_(Convertible<U, T>)>
            static constexpr std::array<std::remove_const_t<T>, Capacity>
            unsafe_recast_init_list(std::initializer_list<U>& il) noexcept {
                assert(
                  il.size() <= capacity()
                  && "trying to construct storage from an initializer_list whose size exceeds the storage capacity");
                std::array<std::remove_const_t<T>, Capacity> d_{};
                for(size_t i = 0, e = il.size(); i < e; ++i) {
                    index(d_, i) = index(il, i);
                }
                return d_;
            }

        public:
            /// Constructor from initializer list.
            ///
            /// Contract: `il.size() <= capacity()`.
            template<typename U, SV_REQUIRES_(Convertible<U, T>)>
            constexpr trivial(std::initializer_list<U> il) noexcept
              : data_(unsafe_recast_init_list(il)) {
                unsafe_set_size(static_cast<size_type>(il.size()));
            }
        };

        /// Storage for non-trivial elements.
        template<typename T, size_t Capacity>
        struct non_trivial {
            static_assert(!Trivial<T>, "use storage::trivial for Trivial<T> elements");
            static_assert(Capacity != size_t{0}, "Capacity must be greater than zero!");

            /// Smallest size_type that can represent Capacity:
            using size_type       = smallest_size_t<Capacity>;
            using value_type      = T;
            using difference_type = ptrdiff_t;
            using pointer         = T*;
            using const_pointer   = T const*;

        private:
            /// Number of elements allocated in the embedded storage:
            size_type size_ = 0;

            using data_t = std::conditional_t<
              !Const<T>,
              std::array<T, Capacity>,
              std::array<std::remove_const_t<T>, Capacity> const>;
            alignas(alignof(T)) data_t data_{};
            // FIXME: ^ this won't work for types with "broken" alignof
            // like SIMD types (one would also need to provide an
            // overload of operator new to make heap allocations of this
            // type work for these types).

        public:
            /// Direct access to the underlying storage.
            ///
            /// Complexity: O(1) in time and space.
            const_pointer data() const noexcept { return reinterpret_cast<const_pointer>(data_); }

            /// Direct access to the underlying storage.
            ///
            /// Complexity: O(1) in time and space.
            pointer data() noexcept { return reinterpret_cast<pointer>(data_); }

            /// Pointer to one-past-the-end.
            const_pointer end() const noexcept { return data() + size(); }

            /// Pointer to one-past-the-end.
            pointer end() noexcept { return data() + size(); }

            /// Number of elements in the storage.
            ///
            /// Complexity: O(1) in time and space.
            constexpr size_type size() const noexcept { return size_; }

            /// Maximum number of elements that can be allocated in the
            /// storage.
            ///
            /// Complexity: O(1) in time and space.
            static constexpr size_type capacity() noexcept { return Capacity; }

            /// Is the storage empty?
            constexpr bool empty() const noexcept { return size() == size_type{0}; }

            /// Is the storage full?
            constexpr bool full() const noexcept { return size() == Capacity; }

            /// Constructs an element in-place at the end of the
            /// embedded storage.
            ///
            /// Complexity: O(1) in time and space.
            /// Contract: the storage is not full.
            template<typename... Args, SV_REQUIRES_(Constructible<T, Args...>)>
            void emplace_back(Args&&... args) noexcept(noexcept(new(end())
                                                                  T(std::forward<Args>(args)...))) {
                assert(!full() && "tried to emplace_back on full storage");
                new(end()) T(std::forward<Args>(args)...);
                unsafe_set_size(size() + 1);
            }

            /// Remove the last element from the container.
            ///
            /// Complexity: O(1) in time and space.
            /// Contract: the storage is not empty.
            void pop_back() noexcept(std::is_nothrow_destructible_v<T>) {
                assert(!empty() && "tried to pop_back from empty storage!");
                auto ptr = end() - 1;
                ptr->~T();
                unsafe_set_size(size() - 1);
            }

            /// (unsafe) Changes the container size to \p new_size.
            ///
            /// Contract: `new_size <= capacity()`.
            /// \warning No elements are constructed or destroyed.
            constexpr void unsafe_set_size(size_t new_size) noexcept {
                assert(new_size <= Capacity && "new_size out-of-bounds [0, Capacity)");
                size_ = size_type(new_size);
            }

            /// (unsafe) Destroy elements in the range [begin, end).
            ///
            /// \warning: The size of the storage is not changed.
            template<typename InputIt, SV_REQUIRES_(InputIterator<InputIt>)>
            void unsafe_destroy(InputIt first, InputIt last) noexcept(
              std::is_nothrow_destructible_v<T>) {
                assert(first >= data() && first <= end() && "first is out-of-bounds");
                assert(last >= data() && last <= end() && "last is out-of-bounds");
                for(; first != last; ++first) {
                    first->~T();
                }
            }

            /// (unsafe) Destroys all elements of the storage.
            ///
            /// \warning: The size of the storage is not changed.
            void unsafe_destroy_all() noexcept(std::is_nothrow_destructible_v<T>) {
                unsafe_destroy(data(), end());
            }

            constexpr non_trivial()                              = default;
            constexpr non_trivial(non_trivial const&)            = default;
            constexpr non_trivial& operator=(non_trivial const&) = default;
            constexpr non_trivial(non_trivial&&) noexcept(std::is_nothrow_move_constructible_v<T>)
              = default;
            constexpr non_trivial&
            operator=(non_trivial&&) noexcept(std::is_nothrow_move_assignable_v<T>)
              = default;
            ~non_trivial() noexcept(std::is_nothrow_destructible_v<T>) { unsafe_destroy_all(); }

            /// Constructor from initializer list.
            ///
            /// Contract: `il.size() <= capacity()`.
            template<typename U, SV_REQUIRES_(Convertible<U, T>)>
            constexpr non_trivial(std::initializer_list<U> il) noexcept(
              noexcept(emplace_back(index(il, 0)))) {
                assert(
                  il.size() <= capacity()
                  && "trying to construct storage from an initializer_list whose size exceeds the storage capacity");
                for(size_t i = 0; i < il.size(); ++i) {
                    emplace_back(index(il, i));
                }
            }
        };

        /// Selects the vector storage.
        template<typename T, size_t Capacity>
        using _t = std::conditional_t<
          Capacity == 0,
          zero_sized<T>,
          std::conditional_t<Trivial<T>, trivial<T, Capacity>, non_trivial<T, Capacity>>>;

    }   // namespace storage

}   // namespace sv_detail

/// Dynamically-resizable fixed-capacity vector.
template<typename T, size_t Capacity>
struct StaticVector : private sv_detail::storage::_t<T, Capacity> {
private:
    static_assert(std::is_nothrow_destructible_v<T>, "T must be nothrow destructible");
    using base_t = sv_detail::storage::_t<T, Capacity>;
    using self   = StaticVector<T, Capacity>;

    using base_t::unsafe_destroy;
    using base_t::unsafe_destroy_all;
    using base_t::unsafe_set_size;

public:
    using value_type             = typename base_t::value_type;
    using difference_type        = ptrdiff_t;
    using reference              = value_type&;
    using const_reference        = value_type const&;
    using pointer                = typename base_t::pointer;
    using const_pointer          = typename base_t::const_pointer;
    using iterator               = typename base_t::pointer;
    using const_iterator         = typename base_t::const_pointer;
    using size_type              = size_t;
    using reverse_iterator       = ::std::reverse_iterator<iterator>;
    using const_reverse_iterator = ::std::reverse_iterator<const_iterator>;

    /// \name Size / capacity
    ///@{
    using base_t::empty;
    using base_t::full;

    /// Number of elements in the vector
    constexpr size_type size() const noexcept { return base_t::size(); }

    /// Maximum number of elements that can be allocated in the vector
    static constexpr size_type capacity() noexcept { return base_t::capacity(); }

    /// Maximum number of elements that can be allocated in the vector
    static constexpr size_type max_size() noexcept { return capacity(); }

    ///@} // Size / capacity

    /// \name Data access
    ///@{

    using base_t::data;

    ///@} // Data access

    /// \name Iterators
    ///@{

    constexpr iterator       begin() noexcept { return data(); }
    constexpr const_iterator begin() const noexcept { return data(); }
    constexpr iterator       end() noexcept { return data() + size(); }
    constexpr const_iterator end() const noexcept { return data() + size(); }

    reverse_iterator       rbegin() noexcept { return reverse_iterator(end()); }
    const_reverse_iterator rbegin() const noexcept { return const_reverse_iterator(end()); }
    reverse_iterator       rend() noexcept { return reverse_iterator(begin()); }
    const_reverse_iterator rend() const noexcept { return const_reverse_iterator(begin()); }

    constexpr const_iterator cbegin() noexcept { return begin(); }
    constexpr const_iterator cbegin() const noexcept { return begin(); }
    constexpr const_iterator cend() noexcept { return end(); }
    constexpr const_iterator cend() const noexcept { return end(); }

    ///@}  // Iterators

private:
    /// \name Iterator bound-check utilites
    ///@{

    template<typename It>
    constexpr void assert_iterator_in_range(It it) noexcept {
        static_assert(sv_detail::Pointer<It>);
        assert(begin() <= it && "iterator not in range");
        assert(it <= end() && "iterator not in range");
    }

    template<typename It0, typename It1>
    constexpr void assert_valid_iterator_pair(It0 first, It1 last) noexcept {
        static_assert(sv_detail::Pointer<It0>);
        static_assert(sv_detail::Pointer<It1>);
        assert(first <= last && "invalid iterator pair");
    }

    template<typename It0, typename It1>
    constexpr void assert_iterator_pair_in_range(It0 first, It1 last) noexcept {
        assert_iterator_in_range(first);
        assert_iterator_in_range(last);
        assert_valid_iterator_pair(first, last);
    }

    ///@}
public:
    /// \name Element access
    ///
    ///@{

    /// Unchecked access to element at index \p pos (UB if index not in
    /// range)
    constexpr reference operator[](size_type pos) noexcept { return sv_detail::index(*this, pos); }

    /// Unchecked access to element at index \p pos (UB if index not in
    /// range)
    constexpr const_reference operator[](size_type pos) const noexcept {
        return sv_detail::index(*this, pos);
    }

    ///
    constexpr reference       front() noexcept { return sv_detail::index(*this, 0); }
    constexpr const_reference front() const noexcept { return sv_detail::index(*this, 0); }

    constexpr reference back() noexcept {
        assert(!empty() && "calling back on an empty vector");
        return sv_detail::index(*this, size() - 1);
    }
    constexpr const_reference back() const noexcept {
        assert(!empty() && "calling back on an empty vector");
        return sv_detail::index(*this, size() - 1);
    }

    ///@} // Element access

    /// \name Modifiers
    ///@{

    using base_t::emplace_back;
    using base_t::pop_back;

    /// Clears the vector.
    constexpr void clear() noexcept {
        unsafe_destroy_all();
        unsafe_set_size(0);
    }

    /// Appends \p value at the end of the vector.
    template<
      typename U,
      SV_REQUIRES_(sv_detail::Constructible<T, U>&& sv_detail::Assignable<reference, U&&>)>
    constexpr void push_back(U&& value) noexcept(noexcept(emplace_back(std::forward<U>(value)))) {
        assert(!full() && "vector is full!");
        emplace_back(std::forward<U>(value));
    }

    /// Appends a default constructed `T` at the end of the vector.
    SV_REQUIRES(sv_detail::Constructible<T, T>&& sv_detail::Assignable<reference, T&&>)
    void push_back() noexcept(noexcept(emplace_back(T{}))) {
        assert(!full() && "vector is full!");
        emplace_back(T{});
    }

    template<typename... Args, SV_REQUIRES_(sv_detail::Constructible<T, Args...>)>
    constexpr iterator emplace(const_iterator position, Args&&... args) noexcept(
      noexcept(move_insert(position, std::declval<value_type*>(), std::declval<value_type*>()))) {
        assert(!full() && "tried emplace on full StaticVector!");
        assert_iterator_in_range(position);
        value_type a(std::forward<Args>(args)...);
        return move_insert(position, &a, &a + 1);
    }
    SV_REQUIRES(sv_detail::CopyConstructible<T>)
    constexpr iterator insert(const_iterator position, const_reference x) noexcept(
      noexcept(insert(position, size_type(1), x))) {
        assert(!full() && "tried insert on full StaticVector!");
        assert_iterator_in_range(position);
        return insert(position, size_type(1), x);
    }

    SV_REQUIRES(sv_detail::MoveConstructible<T>)
    constexpr iterator insert(const_iterator position, value_type&& x) noexcept(
      noexcept(move_insert(position, &x, &x + 1))) {
        assert(!full() && "tried insert on full StaticVector!");
        assert_iterator_in_range(position);
        return move_insert(position, &x, &x + 1);
    }

    SV_REQUIRES(sv_detail::CopyConstructible<T>)
    constexpr iterator
    insert(const_iterator position, size_type n, T const& x) noexcept(noexcept(push_back(x))) {
        assert_iterator_in_range(position);
        auto const new_size = size() + n;
        assert(new_size <= capacity() && "trying to insert beyond capacity!");
        auto b = end();
        while(n != 0) {
            push_back(x);
            --n;
        }

        auto writable_position = begin() + (position - begin());
        sv_detail::slow_rotate(writable_position, b, end());
        return writable_position;
    }

    template<
      class InputIt,
      SV_REQUIRES_(sv_detail::InputIterator<InputIt>and sv_detail::
                     Constructible<value_type, sv_detail::iterator_reference_t<InputIt>>)>
    constexpr iterator insert(const_iterator position, InputIt first, InputIt last) noexcept(
      noexcept(emplace_back(*first))) {
        assert_iterator_in_range(position);
        assert_valid_iterator_pair(first, last);
        if constexpr(sv_detail::RandomAccessIterator<InputIt>) {
            assert(
              size() + static_cast<size_type>(last - first) <= capacity()
              && "trying to insert beyond capacity!");
        }
        auto b = end();

        // insert at the end and then just rotate:
        // cannot use try in constexpr function
        // try {  // if copy_constructor throws you get basic-guarantee?
        for(; first != last; ++first) {
            emplace_back(*first);
        }
        // } catch (...) {
        //   erase(b, end());
        //   throw;
        // }

        auto writable_position = begin() + (position - begin());
        sv_detail::slow_rotate(writable_position, b, end());
        return writable_position;
    }

    template<class InputIt, SV_REQUIRES_(sv_detail::InputIterator<InputIt>)>
    constexpr iterator move_insert(const_iterator position, InputIt first, InputIt last) noexcept(
      noexcept(emplace_back(move(*first)))) {
        assert_iterator_in_range(position);
        assert_valid_iterator_pair(first, last);
        if constexpr(sv_detail::RandomAccessIterator<InputIt>) {
            assert(
              size() + static_cast<size_type>(last - first) <= capacity()
              && "trying to insert beyond capacity!");
        }
        iterator b = end();

        // we insert at the end and then just rotate:
        for(; first != last; ++first) {
            emplace_back(move(*first));
        }
        auto writable_position = begin() + (position - begin());
        sv_detail::slow_rotate<iterator>(writable_position, b, end());
        return writable_position;
    }

    SV_REQUIRES(sv_detail::CopyConstructible<T>)
    constexpr iterator insert(const_iterator position, std::initializer_list<T> il) noexcept(
      noexcept(insert(position, il.begin(), il.end()))) {
        assert_iterator_in_range(position);
        return insert(position, il.begin(), il.end());
    }

    SV_REQUIRES(sv_detail::Movable<value_type>)
    constexpr iterator erase(const_iterator position) noexcept {
        assert_iterator_in_range(position);
        return erase(position, position + 1);
    }

    SV_REQUIRES(sv_detail::Movable<value_type>)
    constexpr iterator erase(const_iterator first, const_iterator last) noexcept {
        assert_iterator_pair_in_range(first, last);
        iterator p = begin() + (first - begin());
        if(first != last) {
            unsafe_destroy(sv_detail::move(p + (last - first), end(), p), end());
            unsafe_set_size(size() - static_cast<size_type>(last - first));
        }

        return p;
    }

    SV_REQUIRES(sv_detail::Assignable<T&, T&&>)
    constexpr void swap(StaticVector& other) noexcept(std::is_nothrow_swappable_v<T>) {
        StaticVector tmp = move(other);
        other            = move(*this);
        (*this)          = move(tmp);
    }

    /// Resizes the container to contain \p sz elements. If elements
    /// need to be appended, these are copy-constructed from \p value.
    ///
    SV_REQUIRES(sv_detail::CopyConstructible<T>)
    constexpr void
    resize(size_type sz, T const& value) noexcept(std::is_nothrow_copy_constructible_v<T>) {
        if(sz == size()) {
            return;
        }
        if(sz > size()) {
            assert(
              sz <= capacity() && "StaticVector cannot be resized to a size greater than capacity");
            insert(end(), sz - size(), value);
        } else {
            erase(end() - (size() - sz), end());
        }
    }

private:
    SV_REQUIRES(sv_detail::MoveConstructible<T> or sv_detail::CopyConstructible<T>)
    constexpr void emplace_n(size_type n) noexcept(
      (sv_detail::MoveConstructible<T> && std::is_nothrow_move_constructible_v<T>)
      || (sv_detail::CopyConstructible<T> && std::is_nothrow_copy_constructible_v<T>)) {
        assert(n <= capacity() && "StaticVector cannot be resized to a size greater than capacity");
        while(n != size()) {
            emplace_back(T{});
        }
    }

public:
    /// Resizes the container to contain \p sz elements. If elements
    /// need to be appended, these are move-constructed from `T{}` (or
    /// copy-constructed if `T` is not `sv_detail::MoveConstructible`).
    SV_REQUIRES(sv_detail::Movable<value_type>)
    constexpr void resize(size_type sz) noexcept(
      (sv_detail::MoveConstructible<T> && std::is_nothrow_move_constructible_v<T>)
      || (sv_detail::CopyConstructible<T> && std::is_nothrow_copy_constructible_v<T>)) {
        if(sz == size()) {
            return;
        }

        if(sz > size()) {
            emplace_n(sz);
        } else {
            erase(end() - (size() - sz), end());
        }
    }

    ///@}  // Modifiers

    /// \name Construct/copy/move/destroy
    ///@{

    /// Default constructor.
    constexpr StaticVector() = default;

    /// Copy constructor.
    SV_REQUIRES(sv_detail::CopyConstructible<value_type>)
    constexpr StaticVector(StaticVector const& other) noexcept(
      noexcept(insert(begin(), other.begin(), other.end()))) {
        // nothin to assert: size of other cannot exceed capacity
        // because both vectors have the same type
        insert(begin(), other.begin(), other.end());
    }

    /// Move constructor.
    SV_REQUIRES(sv_detail::MoveConstructible<value_type>)
    constexpr StaticVector(StaticVector&& other) noexcept(
      noexcept(move_insert(begin(), other.begin(), other.end()))) {
        // nothin to assert: size of other cannot exceed capacity
        // because both vectors have the same type
        move_insert(begin(), other.begin(), other.end());
    }

    /// Copy assignment.
    SV_REQUIRES(sv_detail::Assignable<reference, const_reference>)
    constexpr StaticVector& operator=(StaticVector const& other) noexcept(
      noexcept(clear()) && noexcept(insert(begin(), other.begin(), other.end()))) {
        // nothin to assert: size of other cannot exceed capacity
        // because both vectors have the same type
        clear();
        insert(this->begin(), other.begin(), other.end());
        return *this;
    }

    /// Move assignment.
    SV_REQUIRES(sv_detail::Assignable<reference, reference>)
    constexpr StaticVector& operator=(StaticVector&& other) noexcept(
      noexcept(clear()) and noexcept(move_insert(begin(), other.begin(), other.end()))) {
        // nothin to assert: size of other cannot exceed capacity
        // because both vectors have the same type
        clear();
        move_insert(this->begin(), other.begin(), other.end());
        return *this;
    }

    /// Initializes vector with \p n default-constructed elements.
    SV_REQUIRES(sv_detail::CopyConstructible<T> or sv_detail::MoveConstructible<T>)
    explicit constexpr StaticVector(size_type n) noexcept(noexcept(emplace_n(n))) {
        assert(n <= capacity() && "size exceeds capacity");
        emplace_n(n);
    }

    /// Initializes vector with \p n with \p value.
    SV_REQUIRES(sv_detail::CopyConstructible<T>)
    constexpr StaticVector(size_type n, T const& value) noexcept(
      noexcept(insert(begin(), n, value))) {
        assert(n <= capacity() && "size exceeds capacity");
        insert(begin(), n, value);
    }

    /// Initialize vector from range [first, last).
    template<class InputIt, SV_REQUIRES_(sv_detail::InputIterator<InputIt>)>
    constexpr StaticVector(InputIt first, InputIt last) {
        if constexpr(sv_detail::RandomAccessIterator<InputIt>) {
            assert(last - first >= 0);
            assert(
              static_cast<size_type>(last - first) <= capacity() && "range size exceeds capacity");
        }
        insert(begin(), first, last);
    }

    template<typename U, SV_REQUIRES_(sv_detail::Convertible<U, value_type>)>
    constexpr StaticVector(std::initializer_list<U> il) noexcept(noexcept(base_t(move(il))))
      : base_t(move(il)) {   // assert happens in base_t constructor
    }

    template<class InputIt, SV_REQUIRES_(sv_detail::InputIterator<InputIt>)>
    constexpr void assign(InputIt first, InputIt last) noexcept(
      noexcept(clear()) and noexcept(insert(begin(), first, last))) {
        if constexpr(sv_detail::RandomAccessIterator<InputIt>) {
            assert(last - first >= 0);
            assert(
              static_cast<size_type>(last - first) <= capacity() && "range size exceeds capacity");
        }
        clear();
        insert(begin(), first, last);
    }

    SV_REQUIRES(sv_detail::CopyConstructible<T>)
    constexpr void assign(size_type n, T const& u) {
        assert(n <= capacity() && "size exceeds capacity");
        clear();
        insert(begin(), n, u);
    }
    SV_REQUIRES(sv_detail::CopyConstructible<T>)
    constexpr void assign(std::initializer_list<T> const& il) {
        assert(il.size() <= capacity() && "initializer_list size exceeds capacity");
        clear();
        insert(this->begin(), il.begin(), il.end());
    }
    SV_REQUIRES(sv_detail::CopyConstructible<T>)
    constexpr void assign(std::initializer_list<T>&& il) {
        assert(il.size() <= capacity() && "initializer_list size exceeds capacity");
        clear();
        insert(this->begin(), il.begin(), il.end());
    }

    ///@}  // Construct/copy/move/destroy/assign
};

template<typename T, size_t Capacity>
constexpr bool
operator==(StaticVector<T, Capacity> const& a, StaticVector<T, Capacity> const& b) noexcept {
    return a.size() == b.size()
       and sv_detail::cmp(a.begin(), a.end(), b.begin(), b.end(), std::equal_to<>{});
}

template<typename T, size_t Capacity>
constexpr bool
operator<(StaticVector<T, Capacity> const& a, StaticVector<T, Capacity> const& b) noexcept {
    return sv_detail::cmp(a.begin(), a.end(), b.begin(), b.end(), std::less<>{});
}

template<typename T, size_t Capacity>
constexpr bool
operator!=(StaticVector<T, Capacity> const& a, StaticVector<T, Capacity> const& b) noexcept {
    return not(a == b);
}

template<typename T, size_t Capacity>
constexpr bool
operator<=(StaticVector<T, Capacity> const& a, StaticVector<T, Capacity> const& b) noexcept {
    return sv_detail::cmp(a.begin(), a.end(), b.begin(), b.end(), std::less_equal<>{});
}

template<typename T, size_t Capacity>
constexpr bool
operator>(StaticVector<T, Capacity> const& a, StaticVector<T, Capacity> const& b) noexcept {
    return sv_detail::cmp(a.begin(), a.end(), b.begin(), b.end(), std::greater<>{});
}

template<typename T, size_t Capacity>
constexpr bool
operator>=(StaticVector<T, Capacity> const& a, StaticVector<T, Capacity> const& b) noexcept {
    return sv_detail::cmp(a.begin(), a.end(), b.begin(), b.end(), std::greater_equal<>{});
}

}   // namespace Kvasir

// undefine all the internal macros
#undef SV_CONCEPT_PP_CAT_
#undef SV_CONCEPT_PP_CAT
#undef SV_REQUIRES_
#undef SV_REQUIRES
