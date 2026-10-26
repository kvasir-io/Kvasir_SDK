#pragma once
#include "Exec.hpp"
#include "Types.hpp"
#include "Utility.hpp"
#include "kvasir/Common/Tags.hpp"
#include "kvasir/Mpl/Types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

// apply(): the planner of the Register layer. The arguments (actions, lists of actions, sequencePoints) are
// flattened into one pack; a constexpr plan (Detail::Plan) orders and merges them into accesses, and one comma
// fold executes the accesses, each through the hook ExecuteSeam<Tag::User, action> (Exec.hpp) with the OR of its
// runtime values (picked from the arguments by pack index). Everything on the execution path is always inlined, with
// no lambda and no std:: call: RAM functions call apply() (gcc -Og keeps a lambda or a plain inline helper out of
// line, in flash).
//
// Execution order: steps (separated by sequencePoints) run in order. Within a step, accesses run by register
// address descending, then reads before toggles before writes, and actions of equal (address, kind rank) in
// argument order (after flattening). A write that must follow another goes into its own apply() or after a
// sequencePoint. Merging is between neighbours of that order (walked ascending, the merged access as the "last"),
// of the same Address TYPE only (a hand-built Address of the same number stays apart), judged by the table at
// mergeOf(). Tests: tests/apply_order_tests.cpp, tests/apply_plan_tests.cpp, tests/merge_tests.cpp.

namespace Kvasir { namespace Register {

    // R::overrideDefaults(...) with a runtime value among its actions (Register.hpp): defined with its apply() at the
    // end of this file
    template<typename Defaults, typename... Ts>
    struct Defaulted;

    namespace Detail {
        // Two writes in one step that cover the same bits of a register, at least one of them a runtime value: the
        // merged write is the OR of both, which is never what was meant. Bits the register ignores a zero on are
        // exempt (two pins' runtime masks into one SET register).
        constexpr bool runtimeDisjoint(unsigned mask1,
                                       unsigned mask2,
                                       unsigned ignoredIfZero) {
            return (mask1 & mask2 & ~ignoredIfZero) == 0;
        }

        // A literal and a runtime write of the same bits merge into the OR of both: right only when the literal
        // writes no 1 there (R::overrideDefaults with a runtime value merges a field's 0 default with that value -
        // that is fine; write(F, value<2>()), write(F, x) is not).
        constexpr bool literalLeavesRuntimeBits(unsigned literalValue,
                                                unsigned runtimeMask,
                                                unsigned ignoredIfZero) {
            return (literalValue & runtimeMask & ~ignoredIfZero) == 0;
        }

        // Two literal writes merge by OR, which is only right where they agree: a set and a clear of one bit, or
        // two writes of one field with different values, would otherwise merge into whichever has more ones.
        // Same-value overlap is idempotent and stays legal, and so is a disagreement on bits the register ignores
        // a zero on (the SIO's set, clear and xor aliases, write-one-to-clear flags): there the OR is exactly the
        // hardware's own semantics, and two pins' set actions merge into one write on purpose.
        constexpr bool literalsAgree(unsigned mask1,
                                     unsigned value1,
                                     unsigned mask2,
                                     unsigned value2,
                                     unsigned ignoredIfZero = 0) {
            return ((value1 ^ value2) & mask1 & mask2 & ~ignoredIfZero) == 0;
        }

        // A merged action addresses several fields at once: it has no single access kind and no field type, only
        // the register's. Nothing downstream reads either from it (the executors take the masks and the value).
        template<typename TAddress, unsigned Mask>
        using MergedLocation
          = FieldLocation<TAddress, Mask, ReadWriteAccess, typename TAddress::RegType>;

        // ---- the arguments ----------------------------------------------------------------------------------

        template<typename T>
        inline constexpr bool isApplyArg = false;

        template<typename L, typename A>
        inline constexpr bool isApplyArg<Action<L, A>> = true;

        template<>
        inline constexpr bool isApplyArg<SequencePoint> = true;

        template<typename... Ts>
        inline constexpr bool isApplyArg<brigand::list<Ts...>> = (isApplyArg<Ts> && ...);

        // a Defaulted bundle, or a list holding one: only apply(bundle) takes it, alone
        template<typename T>
        inline constexpr bool holdsDefaulted = false;

        template<typename D, typename... Ts>
        inline constexpr bool holdsDefaulted<Defaulted<D, Ts...>> = true;

        template<typename... Ts>
        inline constexpr bool holdsDefaulted<brigand::list<Ts...>> = (holdsDefaulted<Ts> || ...);

        // one flattened action (or sequencePoint) with the index of the argument it came from; Top: the argument
        // itself, not an element of a list (only those may carry a runtime value)
        template<typename TAction, std::size_t Arg, bool Top>
        struct Flat {
            using action = TAction;
        };

        template<std::size_t Arg, bool Top, typename T>
        struct FlattenArg {
            using type = brigand::list<Flat<T, Arg, Top>>;
        };

        template<std::size_t Arg, bool Top, typename... Ts>
        struct FlattenArg<Arg, Top, brigand::list<Ts...>> {
            using type
              = brigand::append<brigand::list<>, typename FlattenArg<Arg, false, Ts>::type...>;
        };

        template<typename TSeq, typename... Args>
        struct FlattenArgs;

        template<std::size_t... I, typename... Args>
        struct FlattenArgs<std::index_sequence<I...>, Args...> {
            using type
              = brigand::append<brigand::list<>, typename FlattenArg<I, true, Args>::type...>;
        };

        // ---- one record per flattened action ----------------------------------------------------------------

        enum class Kind : std::uint8_t {
            sequencePoint,
            read,
            literal,
            atomic,
            toggle,
            runtime,
            literalAndRuntime,
            other   // a chip's own action kind: never merged
        };

        // One row per action kind: what the plan reads from an action (kind, literal value, runtime bits) and how
        // a merged access of that kind is rebuilt (as<value, runtime bits>). A chip's own kind is `other`, never
        // merged.
        template<typename TKind>
        struct KindOf {
            static constexpr Kind     kind        = Kind::other;
            static constexpr unsigned value       = 0;
            static constexpr unsigned runtimeMask = 0;
        };

        template<>
        struct KindOf<ReadAction> : KindOf<void> {
            static constexpr Kind kind = Kind::read;
            template<unsigned, unsigned>
            using as = ReadAction;
        };

        template<unsigned V>
        struct KindOf<WriteLiteralAction<V>> : KindOf<void> {
            static constexpr Kind     kind  = Kind::literal;
            static constexpr unsigned value = V;
            template<unsigned W, unsigned>
            using as = WriteLiteralAction<W>;
        };

        template<unsigned V>
        struct KindOf<AtomicWriteLiteralAction<V>> : KindOf<void> {
            static constexpr Kind     kind  = Kind::atomic;
            static constexpr unsigned value = V;
            template<unsigned W, unsigned>
            using as = AtomicWriteLiteralAction<W>;
        };

        template<unsigned V>
        struct KindOf<XorLiteralAction<V>> : KindOf<void> {
            static constexpr Kind     kind  = Kind::toggle;
            static constexpr unsigned value = V;
            template<unsigned W, unsigned>
            using as = XorLiteralAction<W>;
        };

        template<>
        struct KindOf<WriteAction> : KindOf<void> {
            static constexpr Kind kind = Kind::runtime;
            template<unsigned, unsigned>
            using as = WriteAction;
        };

        template<unsigned V, unsigned R>
        struct KindOf<WriteRuntimeAndLiteralAction<V, R>> : KindOf<void> {
            static constexpr Kind     kind        = Kind::literalAndRuntime;
            static constexpr unsigned value       = V;
            static constexpr unsigned runtimeMask = R;
            template<unsigned W, unsigned S>
            using as = WriteRuntimeAndLiteralAction<W, S>;
        };

        template<typename... Rows>
        consteval bool kindsInOrder() {
            unsigned k = static_cast<unsigned>(Kind::read);
            return ((KindOf<Rows>::kind == static_cast<Kind>(k++)) && ...);
        }

        // the action a merged access of kind K executes: K's row of the table, found through one action of each
        // mergeable kind in the order of Kind
        template<typename... Rows>
        struct KindTable {
            static_assert(kindsInOrder<Rows...>());

            template<Kind K, unsigned V, unsigned R>
            using type =
              typename KindOf<Rows...[static_cast<unsigned>(K)
                                      - static_cast<unsigned>(Kind::read)]>::template as<V, R>;
        };

        template<Kind K, unsigned V, unsigned R>
        using KindType =
          typename KindTable<ReadAction,
                             WriteLiteralAction<0>,
                             AtomicWriteLiteralAction<0>,
                             XorLiteralAction<0>,
                             WriteAction,
                             WriteRuntimeAndLiteralAction<0, 0>>::template type<K, V, R>;

        inline constexpr unsigned noArg = ~0U;

        struct Rec {
            Kind     kind{Kind::sequencePoint};
            unsigned address{};
            unsigned ignoredIfZero{};   // the register's write-ignored-if-zero mask
            unsigned rank{};            // reads 3, toggles 2, every write 1
            unsigned mask{};
            unsigned value{};
            unsigned runtimeMask{};
            unsigned arg{noArg};    // the argument index of a runtime value, else noArg
            bool     fromReset{};   // the Address is Register::fromReset's
        };

        template<typename TFlat>
        struct RecOf;

        template<std::size_t Arg, bool Top>
        struct RecOf<Flat<SequencePoint, Arg, Top>> {
            static constexpr Rec rec{};
        };

        template<typename TAddress,
                 unsigned Mask,
                 typename TAccess,
                 typename TFieldType,
                 typename TKind,
                 std::size_t Arg,
                 bool        Top>
        struct RecOf<
          Flat<Action<FieldLocation<TAddress, Mask, TAccess, TFieldType>, TKind>, Arg, Top>> {
            static constexpr Kind kind = KindOf<TKind>::kind;
            static_assert(
              Top || kind != Kind::runtime,
              "runtime values can only be executed in an apply, they cannot be stored in a list");
            static constexpr Rec rec{kind,
                                     RegisterTraits<TAddress>::address,
                                     RegisterTraits<TAddress>::writeIgnoredIfZeroMask,
                                     kind == Kind::read     ? 3U
                                     : kind == Kind::toggle ? 2U
                                                            : 1U,
                                     Mask,
                                     KindOf<TKind>::value,
                                     KindOf<TKind>::runtimeMask,
                                     kind == Kind::runtime ? static_cast<unsigned>(Arg) : noArg,
                                     RegisterTraits<TAddress>::fromReset};
        };

        template<typename TAction>
        struct AddressOf;

        template<typename TAddress,
                 unsigned Mask,
                 typename TAccess,
                 typename TFieldType,
                 typename TKind>
        struct AddressOf<Action<FieldLocation<TAddress, Mask, TAccess, TFieldType>, TKind>> {
            using type = TAddress;
        };

        // ---- the merge table --------------------------------------------------------------------------------

        // What two actions of one register in one step become when they meet in the walk (only neighbours of the
        // execution order meet, see the order at the top). Next is the next action of the ascending walk, last
        // what has been merged so far. A pair without a row stays two accesses.
        //
        //   next \ last   read   literal   runtime   lit+rt   toggle   atomic
        //   read          read   -         -         -        -        -
        //   literal       -      lit [A]   lit+rt[L] lit+rt[A] -       -
        //   runtime       -      lit+rt[L] rt [D]    lit+rt[D,L] [O]   [O]
        //   toggle        -      -         [O]       [O]      xor[T]   -
        //   atomic        -      -         [O]       [O]      -        atomic[A]
        //
        // [A] literalsAgree, [D] runtimeDisjoint, [L] literalLeavesRuntimeBits, [T] two toggles of different bits
        // only, [O] a toggle or an atomic write never merges with a runtime write (the toggle would become an OR,
        // the atomic write lose its executor); of the same bits it is an error. A merged literal+runtime write
        // carries the runtime bits it holds, so a later runtime write is checked against them.
        enum class MergeError : std::uint8_t {
            none,
            literalsDisagree,
            twoToggles,
            runtimeOverlap,
            literalOverRuntime,
            toggleOrAtomicOverRuntime,
            fromResetAndPlainWrite
        };

        struct Merged {
            bool       merges{};
            Kind       kind{};
            unsigned   value{};
            unsigned   runtimeMask{};
            MergeError error{MergeError::none};
        };

        struct Head {
            Kind     kind{};
            unsigned mask{};
            unsigned value{};
            unsigned runtimeMask{};
        };

        constexpr bool toggleOrAtomic(Kind k) { return k == Kind::toggle || k == Kind::atomic; }

        constexpr unsigned runtimeBitsOf(Kind     k,
                                         unsigned mask,
                                         unsigned runtimeMask) {
            return k == Kind::runtime ? mask : k == Kind::literalAndRuntime ? runtimeMask : 0U;
        }

        consteval Merged mergeOf(Rec const&  next,
                                 Head const& last) {
            using enum Kind;
            Kind const     n    = next.kind;
            Kind const     l    = last.kind;
            unsigned const ign  = next.ignoredIfZero;
            auto const     fail = [](Merged m, bool ok, MergeError e) {
                if(!ok && m.error == MergeError::none) { m.error = e; }
                return m;
            };
            if(n == read && l == read) { return Merged{true, read}; }
            if((n == literal && l == literal) || (n == atomic && l == atomic)) {
                return fail(Merged{true, n, next.value | last.value},
                            literalsAgree(next.mask, next.value, last.mask, last.value, ign),
                            MergeError::literalsDisagree);
            }
            if(n == toggle && l == toggle) {
                return fail(Merged{true, toggle, next.value | last.value},
                            (next.mask & last.mask) == 0,
                            MergeError::twoToggles);
            }
            if(n == runtime && l == runtime) {
                return fail(Merged{true, runtime},
                            runtimeDisjoint(next.mask, last.mask, ign),
                            MergeError::runtimeOverlap);
            }
            if(n == literal && l == runtime) {
                return fail(Merged{true, literalAndRuntime, next.value, last.mask},
                            literalLeavesRuntimeBits(next.value, last.mask, ign),
                            MergeError::literalOverRuntime);
            }
            if(n == runtime && l == literal) {
                return fail(Merged{true, literalAndRuntime, last.value, next.mask},
                            literalLeavesRuntimeBits(last.value, next.mask, ign),
                            MergeError::literalOverRuntime);
            }
            if(n == literal && l == literalAndRuntime) {
                return fail(
                  Merged{true, literalAndRuntime, next.value | last.value, last.runtimeMask},
                  literalsAgree(next.mask, next.value, last.mask, last.value, ign),
                  MergeError::literalsDisagree);
            }
            if(n == runtime && l == literalAndRuntime) {
                Merged const m{true, literalAndRuntime, last.value, last.runtimeMask | next.mask};
                return fail(fail(m,
                                 runtimeDisjoint(next.mask, last.runtimeMask, ign),
                                 MergeError::runtimeOverlap),
                            literalLeavesRuntimeBits(last.value, next.mask, ign),
                            MergeError::literalOverRuntime);
            }
            unsigned const nRuntime = runtimeBitsOf(n, next.mask, next.runtimeMask);
            unsigned const lRuntime = runtimeBitsOf(l, last.mask, last.runtimeMask);
            if((toggleOrAtomic(n) && lRuntime != 0) || (toggleOrAtomic(l) && nRuntime != 0)) {
                return fail(Merged{false},
                            runtimeDisjoint(toggleOrAtomic(n) ? next.mask : nRuntime,
                                            toggleOrAtomic(l) ? last.mask : lRuntime,
                                            ign),
                            MergeError::toggleOrAtomicOverRuntime);
            }
            return Merged{false};
        }

        // ---- the plan ---------------------------------------------------------------------------------------

        // A read's result slot: one per read access, i.e. per (step, register) - fields of one register read in one
        // step share the access and its slot, a register read in two steps has two. Slots are ordered by step, then
        // address (one step: by address).
        struct ReadKey {
            int      step;
            unsigned address;

            constexpr bool operator<(ReadKey const& o) const {
                return step < o.step || (step == o.step && address < o.address);
            }

            constexpr bool operator==(ReadKey const&) const = default;
        };

        // the actions in sorted order: per step, address ascending, rank ascending, equal (address, rank) in
        // REVERSE flattened order (the key is total, so any sort gives this permutation)
        template<std::size_t N>
        struct Order {
            unsigned    idx[N + 1]{};
            unsigned    step[N + 1]{};   // the step of each flattened element
            std::size_t count{};
        };

        template<std::size_t N>
        consteval Order<N> makeOrder(Rec const (&rec)[N + 1]) {
            Order<N> o{};
            unsigned step = 0;
            bool     used = false;
            for(std::size_t i = 0; i < N; ++i) {
                if(rec[i].kind == Kind::sequencePoint) {
                    if(used) {
                        ++step;
                        used = false;
                    }
                    continue;
                }
                used             = true;
                o.step[i]        = step;
                o.idx[o.count++] = static_cast<unsigned>(i);
            }
            auto const less = [&](unsigned a, unsigned b) {
                if(o.step[a] != o.step[b]) { return o.step[a] < o.step[b]; }
                if(rec[a].address != rec[b].address) { return rec[a].address < rec[b].address; }
                if(rec[a].rank != rec[b].rank) { return rec[a].rank < rec[b].rank; }
                return a > b;
            };
            // bottom-up merge sort over the indices
            unsigned tmp[N + 1]{};
            for(std::size_t width = 1; width < o.count; width *= 2) {
                for(std::size_t first = 0; first < o.count; first += 2 * width) {
                    std::size_t const mid = first + width < o.count ? first + width : o.count;
                    std::size_t const last
                      = first + 2 * width < o.count ? first + 2 * width : o.count;
                    std::size_t a = first;
                    std::size_t b = mid;
                    std::size_t t = first;
                    while(a < mid && b < last) {
                        tmp[t++] = less(o.idx[b], o.idx[a]) ? o.idx[b++] : o.idx[a++];
                    }
                    while(a < mid) { tmp[t++] = o.idx[a++]; }
                    while(b < last) { tmp[t++] = o.idx[b++]; }
                }
                for(std::size_t i = 0; i < o.count; ++i) { o.idx[i] = tmp[i]; }
            }
            return o;
        }

        // the accesses in execution order
        template<std::size_t N>
        struct Accesses {
            struct Access {
                unsigned head{};   // the flattened element that started it (its type if count == 1)
                unsigned count{};   // actions merged into it
                Kind     kind{};
                unsigned mask{};
                unsigned value{};
                unsigned runtimeMask{};
                unsigned step{};
                unsigned address{};
                unsigned
                  inputBegin{};   // its runtime arguments: inputs[inputBegin, inputEnd), ascending
                unsigned inputEnd{};
            };

            Access      acc[N + 1]{};
            unsigned    inputs[N + 1]{};
            std::size_t count{};
            MergeError  error{MergeError::none};
            bool        reads{};
        };

        template<std::size_t N>
        consteval Accesses<N> makeAccesses(Rec const (&rec)[N + 1],
                                           Order<N> const& o,
                                           bool const (&sameAddressAsPrevious)[N + 1]) {
            using Access = typename Accesses<N>::Access;
            Access      walk[N + 1]{};        // in walk order
            unsigned    accOfWalk[N + 1]{};   // the walk access of each flattened element
            std::size_t n = 0;
            Accesses<N> out{};
            for(std::size_t k = 0; k < o.count; ++k) {
                unsigned const e = o.idx[k];
                Rec const&     r = rec[e];
                // a Register::fromReset write next to a plain write of the same register in one step: the plain one
                // would see the fill overwrite the bits it keeps (sorted by address: they are neighbours)
                if(k != 0) {
                    unsigned const p = o.idx[k - 1];
                    if(o.step[p] == o.step[e] && rec[p].address == r.address
                       && rec[p].fromReset != r.fromReset && rec[p].kind != Kind::read
                       && r.kind != Kind::read && out.error == MergeError::none)
                    {
                        out.error = MergeError::fromResetAndPlainWrite;
                    }
                }
                if(n != 0 && o.step[walk[n - 1].head] == o.step[e] && sameAddressAsPrevious[k]) {
                    Access&      last = walk[n - 1];
                    Merged const m
                      = mergeOf(r, Head{last.kind, last.mask, last.value, last.runtimeMask});
                    if(m.error != MergeError::none && out.error == MergeError::none) {
                        out.error = m.error;
                    }
                    if(m.merges) {
                        last.kind        = m.kind;
                        last.mask        = last.mask | r.mask;
                        last.value       = m.value;
                        last.runtimeMask = m.runtimeMask;
                        ++last.count;
                        accOfWalk[e] = static_cast<unsigned>(n - 1);
                        continue;
                    }
                }
                walk[n]
                  = Access{e, 1, r.kind, r.mask, r.value, r.runtimeMask, o.step[e], r.address};
                accOfWalk[e] = static_cast<unsigned>(n);
                ++n;
            }
            // execution order: per step the walk reversed
            unsigned position[N + 1]{};   // walk index -> execution index
            for(std::size_t begin = 0; begin < n;) {
                std::size_t end = begin;
                while(end < n && walk[end].step == walk[begin].step) { ++end; }
                for(std::size_t i = begin; i < end; ++i) {
                    position[i] = static_cast<unsigned>(begin + (end - 1 - i));
                }
                begin = end;
            }
            for(std::size_t i = 0; i < n; ++i) {
                out.acc[position[i]] = walk[i];
                if(walk[i].kind == Kind::read) { out.reads = true; }
            }
            out.count = n;
            // the runtime arguments of each access, ascending (flattened order is argument order)
            unsigned next = 0;
            for(std::size_t a = 0; a < n; ++a) {
                out.acc[a].inputBegin = next;
                for(std::size_t e = 0; e < N; ++e) {
                    if(rec[e].kind != Kind::sequencePoint && rec[e].arg != noArg
                       && position[accOfWalk[e]] == a)
                    {
                        out.inputs[next++] = rec[e].arg;
                    }
                }
                out.acc[a].inputEnd = next;
            }
            return out;
        }

        template<std::size_t N>
        consteval std::size_t countReads(Rec const (&rec)[N + 1]) {
            std::size_t count = 0;
            for(std::size_t e = 0; e < N; ++e) {
                if(rec[e].kind == Kind::read) { ++count; }
            }
            return count;
        }

        // the reads in flattened order: their elements and their keys
        template<std::size_t R>
        struct Reads {
            unsigned element[R + 1]{};
            ReadKey  key[R + 1]{};
        };

        template<std::size_t N,
                 std::size_t R>
        consteval Reads<R> makeReads(Rec const (&rec)[N + 1],
                                     Order<N> const& o) {
            Reads<R>    r{};
            std::size_t k = 0;
            for(std::size_t e = 0; e < N; ++e) {
                if(rec[e].kind == Kind::read) {
                    r.element[k] = static_cast<unsigned>(e);
                    r.key[k++]   = ReadKey{static_cast<int>(o.step[e]), rec[e].address};
                }
            }
            return r;
        }

        template<typename TFlats>
        struct Plan;

        template<typename... Fs>
        struct Plan<brigand::list<Fs...>> {
            static constexpr std::size_t n = sizeof...(Fs);

            static constexpr Rec rec[n + 1] = {RecOf<Fs>::rec..., Rec{}};

            static constexpr Order<n> order = makeOrder<n>(rec);

            template<std::size_t E>
            using Element = typename Fs...[E] ::action;

            template<std::size_t K>
            static constexpr bool sameAddressAsPrevious() {
                if constexpr(K == 0 || K >= order.count) {
                    return false;
                } else {
                    return std::is_same_v<typename AddressOf<Element<order.idx[K]>>::type,
                                          typename AddressOf<Element<order.idx[K - 1]>>::type>;
                }
            }

            template<typename TSeq>
            struct Same;

            template<std::size_t... K>
            struct Same<std::index_sequence<K...>> {
                static constexpr bool value[n + 1] = {sameAddressAsPrevious<K>()..., false};
            };

            static constexpr Accesses<n> plan
              = makeAccesses<n>(rec, order, Same<std::make_index_sequence<n>>::value);

            static constexpr std::size_t readCount = countReads<n>(rec);

            static constexpr Reads<readCount> reads = makeReads<n, readCount>(rec, order);

            // the slot of a read key: the number of distinct keys below it
            static constexpr unsigned slotOf(ReadKey k) {
                unsigned slot = 0;
                for(std::size_t i = 0; i < readCount; ++i) {
                    bool first = true;
                    for(std::size_t j = 0; j < i; ++j) {
                        if(reads.key[j] == reads.key[i]) { first = false; }
                    }
                    if(first && reads.key[i] < k) { ++slot; }
                }
                return slot;
            }

            template<typename TSeq>
            struct ResultOf {
                using type = void;
            };

            template<std::size_t J0, std::size_t... J>
            struct ResultOf<std::index_sequence<J0, J...>> {
                using type
                  = FieldTuple<brigand::list<brigand::uint32_t<slotOf(reads.key[J0])>,
                                             brigand::uint32_t<slotOf(reads.key[J])>...>,
                               brigand::list<typename Element<reads.element[J0]>::location,
                                             typename Element<reads.element[J]>::location...>>;
            };

            // what apply() returns: with reads a FieldTuple of their slots and field locations, else nothing
            using Result = typename ResultOf<std::make_index_sequence<readCount>>::type;
        };

        template<typename... Args>
        using PlanOf = Plan<typename FlattenArgs<std::index_sequence_for<Args...>, Args...>::type>;

        // ---- execution --------------------------------------------------------------------------------------

        // the OR of an access's runtime values, ascending argument index, grouped as (i0 | i1) | ((i2 | i3) | ...)
        template<std::size_t... Is>
        struct Inputs {
            template<typename... Ts>
            [[gnu::always_inline]] static unsigned value(Ts...) {
                return 0U;
            }
        };

        template<std::size_t A>
        struct Inputs<A> {
            template<typename... Ts>
            [[gnu::always_inline]] static unsigned value(Ts... in) {
                return in...[A];
            }
        };

        template<std::size_t A, std::size_t B>
        struct Inputs<A, B> {
            template<typename... Ts>
            [[gnu::always_inline]] static unsigned value(Ts... in) {
                return in...[A] | in...[B];
            }
        };

        template<std::size_t A, std::size_t B, std::size_t C, std::size_t... Rest>
        struct Inputs<A, B, C, Rest...> {
            template<typename... Ts>
            [[gnu::always_inline]] static unsigned value(Ts... in) {
                return in...[A] | in...[B] | Inputs<C, Rest...>::value(in...);
            }
        };

        template<typename P, std::size_t I, typename TSeq>
        struct InputsOfImpl;

        template<typename P, std::size_t I, std::size_t... J>
        struct InputsOfImpl<P, I, std::index_sequence<J...>> {
            using type = Inputs<P::plan.inputs[P::plan.acc[I].inputBegin + J]...>;
        };

        template<bool Lone, typename P, std::size_t I>
        struct AccessActionImpl {   // one action: its own type
            using type = typename P::template Element<P::plan.acc[I].head>;
        };

        template<typename P, std::size_t I>
        struct AccessActionImpl<false, P, I> {   // merged
            static constexpr auto a = P::plan.acc[I];
            using type              = Action<
              MergedLocation<typename AddressOf<typename P::template Element<a.head>>::type,
                             a.mask>,
              KindType<a.kind, a.value, a.runtimeMask>>;
        };

        template<typename P, std::size_t I>
        struct AccessOf {
            static constexpr auto a = P::plan.acc[I];
            using action            = typename AccessActionImpl<a.count == 1, P, I>::type;
            using inputs =
              typename InputsOfImpl<P, I, std::make_index_sequence<a.inputEnd - a.inputBegin>>::
                type;
        };

        // run one access; a read stores its value in its slot, a write's result is dropped
        template<typename TAccess,
                 int Slot,
                 typename TRet>
        [[gnu::always_inline]] inline void execInto(TRet&    ret,
                                                    unsigned in) {
            if constexpr(Slot >= 0) {
                ret.value_[Slot]
                  = ::Kvasir::Port::LateT<ExecuteSeam<::Kvasir::Tag::User, TAccess>>{}(in);
            } else {
                static_cast<void>(
                  ::Kvasir::Port::LateT<ExecuteSeam<::Kvasir::Tag::User, TAccess>>{}(in));
            }
        }

        template<typename P, typename TRet, typename TSeq>
        struct Run;

        template<typename P, typename TRet, std::size_t... I>
        struct Run<P, TRet, std::index_sequence<I...>> {
            template<std::size_t A>
            static constexpr int slotOf() {
                if constexpr(P::plan.acc[A].kind == Kind::read) {
                    return static_cast<int>(P::slotOf(
                      ReadKey{static_cast<int>(P::plan.acc[A].step), P::plan.acc[A].address}));
                } else {
                    return -1;
                }
            }

            template<typename... Ts>
            [[gnu::always_inline]] static TRet run(Ts... in) {
                if constexpr(std::is_void_v<TRet>) {
                    (static_cast<void>(
                       ::Kvasir::Port::LateT<
                         ExecuteSeam<::Kvasir::Tag::User, typename AccessOf<P, I>::action>>{}(
                         AccessOf<P, I>::inputs::value(in...))),
                     ...);
                } else {
                    // not initialised: each slot is written by exactly one read. Zeroed, or left to C++26's
                    // erroneous-behaviour initialisation (gcc's .DEFERRED_INIT), a result of two slots or more
                    // becomes a memset call at gcc -Og on the M0+ - out of line in flash, from a RAM function's
                    // read. A comma fold runs the accesses in order; no array of their results, the same reason.
#if __has_cpp_attribute(indeterminate)
                    [[indeterminate]] TRet ret;
#else
                    TRet ret;
#endif
                    (execInto<typename AccessOf<P, I>::action, slotOf<I>()>(
                       ret,
                       AccessOf<P, I>::inputs::value(in...)),
                     ...);
                    return ret;
                }
            }
        };

        template<typename T>
        [[gnu::always_inline]] inline unsigned argValue(T const& arg) {
            if constexpr(requires { arg.value_; }) {
                return arg.value_;
            } else {
                return 0U;
            }
        }

    }   // namespace Detail

    // Runs the actions; with reads it returns a FieldTuple (result[field], get<I>()), else nothing. The order and
    // merge rules: the comment at the top of this file.
    template<typename... Args>
    [[gnu::always_inline]] inline auto apply(Args... args) {
        if constexpr(!(Detail::isApplyArg<Args> && ...)) {
            static constexpr bool noBundle = !(Detail::holdsDefaulted<Args> || ...);
            KVASIR_STATIC_ASSERT(noBundle, (Diagnostic::DefaultedNotAlone));
            static_assert(!noBundle, "one of the supplied arguments is not supported");
        } else {
            using P = Detail::PlanOf<Args...>;
            using E = Detail::MergeError;
            static_assert(P::plan.error != E::literalsDisagree,
                          "two actions in one step write different values to the same bits of "
                          "one register: a set and a clear of one bit, or two writes of one "
                          "field (separate them with a sequencePoint if both are meant)");
            static_assert(P::plan.error != E::twoToggles,
                          "two toggles of the same bits in one step: one would undo the other "
                          "(separate them with a sequencePoint if both are meant)");
            static_assert(
              P::plan.error != E::runtimeOverlap,
              "two writes in one apply() cover the same bits of one register and at "
              "least one of them is a runtime value: the result would be the OR of both "
              "(separate them with a sequencePoint if both are meant)");
            static_assert(
              P::plan.error != E::literalOverRuntime,
              "a literal and a runtime write in one apply() cover the same bits and the "
              "literal writes ones there: the result would be the OR of both (separate "
              "them with a sequencePoint if both are meant)");
            static_assert(
              P::plan.error != E::fromResetAndPlainWrite,
              "a Register::fromReset write and a plain write of the same register in one step: the "
              "reset value fill of the one would undo the other (name every field in the "
              "fromReset, or "
              "separate them with a sequencePoint)");
            static_assert(
              P::plan.error != E::toggleOrAtomicOverRuntime,
              "a toggle or an atomic write and a runtime write of the same bits in one apply(): "
              "they cannot merge into one access, and as two the runtime write undoes the other "
              "(separate them with a sequencePoint if both are meant)");
            return Detail::Run<P, typename P::Result, std::make_index_sequence<P::plan.count>>::run(
              Detail::argValue(args)...);
        }
    }

    // no parameters is allowed because it could be used in machine generated code
    // it does nothing
    inline void apply() {}

    inline void apply(brigand::list<>) {}

    namespace Detail {
        template<std::size_t I, typename T>
        struct DefaultedSlot {
            T action;
        };

        template<typename Is, typename... Ts>
        struct DefaultedSlots;

        template<std::size_t... Is, typename... Ts>
        struct DefaultedSlots<std::index_sequence<Is...>, Ts...> : DefaultedSlot<Is, Ts>... {};
    }   // namespace Detail

    // The actions given to R::overrideDefaults(...) when one of them carries a runtime value, and Defaults (a
    // brigand::list) the default writes of the fields they leave out. Only apply() takes it, as its only argument: a
    // bundle in a list or next to other arguments is the named error Diagnostic::DefaultedNotAlone.
    template<typename Defaults, typename... Ts>
    struct Defaulted : Detail::DefaultedSlots<std::index_sequence_for<Ts...>, Ts...> {
        [[gnu::always_inline]] constexpr explicit Defaulted(Ts const&... ts)
          : Detail::DefaultedSlots<std::index_sequence_for<Ts...>,
                                   Ts...>{{ts}...} {}
    };

    namespace Detail {
        template<typename Defaults,
                 typename... Ts,
                 std::size_t... Is>
        [[gnu::always_inline]] inline auto applyDefaulted(Defaulted<Defaults,
                                                                    Ts...> const& bundle,
                                                          std::index_sequence<Is...>) {
            return apply(static_cast<DefaultedSlot<Is, Ts> const&>(bundle).action..., Defaults{});
        }
    }   // namespace Detail

    // apply(R::overrideDefaults(write(R::f, x), ...)): exactly apply(write(R::f, x), ..., <the remaining defaults>)
    template<typename Defaults,
             typename... Ts>
    [[gnu::always_inline]] inline auto apply(Defaulted<Defaults,
                                                       Ts...> const& bundle) {
        return Detail::applyDefaulted(bundle, std::index_sequence_for<Ts...>{});
    }

    // always inlined, as apply() itself: a FieldValue wait (Wait.hpp) in a RAM function must not call into flash
    template<typename TField,
             typename TField::DataType Value>
    [[gnu::always_inline]] inline bool fieldEquals(FieldValue<TField,
                                                              Value>) {
        return apply(Action<TField, ReadAction>{}) == Value;
    }
}}   // namespace Kvasir::Register
