#pragma once

// Block gates as Startup resources (kvasir/StartUp/Resources.hpp). A peripheral block's registers work only once
// its gate is open - a reset bit released, a bus clock mask bit set - and a write into a closed block is lost or
// faults. A chip says where its gated blocks are with one table of rows, each row naming a block's address range,
// the gate's id and the exact action that opens it:
//
//     template<>
//     struct Kvasir::Hal::BlockGates<Kvasir::Tag::User> {
//         using type = Hal::BlockTable<Hal::BlockRow<0x40034000, 0x4000, 22, decltype(clear(RESET::uart0))>, ...>;
//     };
//
// From then on, in every Startup list of the image:
//   - an action of a `powerClockEnable` (or `initStepPinConfig`) that is a row's open action - matched by register,
//     mask and value, not by how it is spelled - provides `Hal::Gate<id>`;
//   - every action of the later phases (pins, periphery, interrupts, enable) whose register lies inside a row's
//     block claims that row's gate (every gate of the block, when a block has two), so a driver that writes a block
//     no listed entry opens is Startup's "claimed but provided nowhere", with GateTag's message.
// A row that is `OpenAtBoot` (the gate is open out of reset, or opened by the chip's first init step) is in the
// table for its open action and the front ends (`OpenActions`), but claims nothing. There is no switch: a firmware
// the check rejects lists the missing opener. Only Startup-list actions are checked; a run-time apply() is not.
//
// The lookups are constexpr folds over the rows and leave no code. The hook is a late port hook
// (kvasir/Port/Hooks.hpp): the chip specialises it where every translation unit of the image sees it.

#include "kvasir/Common/Tags.hpp"
#include "kvasir/Port/Hooks.hpp"
#include "kvasir/Register/Types.hpp"
#include "kvasir/StartUp/Resources.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace Kvasir { namespace Hal {
    // many writers of one block; two drivers on one block both open it, which is one opening
    struct GateTag {
        static constexpr bool        sharedClaim    = true;
        static constexpr bool        coreLocal      = false;
        static constexpr bool        mergeIdentical = true;
        static constexpr char const* message
          = "a peripheral writes a block that no listed powerClockEnable opens (the id is the "
            "chip's "
            "gate number, Kvasir::Hal::BlockGates)";
    };

    template<unsigned Id>
    using Gate = Startup::Resource<GateTag, Id>;

    /// One action as data: its register address, and for a literal write its mask and value.
    struct ActionWrite {
        bool          known   = false;   // an Action on one FieldLocation
        bool          literal = false;   // ... that writes a compile-time value
        std::uint32_t address = 0;
        std::uint32_t mask    = 0;
        std::uint32_t value   = 0;

        constexpr bool operator==(ActionWrite const&) const = default;
    };

    /// `actionWriteOf<A>`: the ActionWrite of a register action type A (`known = false` for anything that is not one
    /// Action on one field).
    template<typename A>
    inline constexpr ActionWrite actionWriteOf{};

    template<unsigned Addr,
             unsigned Z,
             unsigned O,
             typename RegType,
             typename Mode,
             unsigned Mask,
             typename Access,
             typename FieldType,
             typename TAction>
    inline constexpr ActionWrite actionWriteOf<Register::Action<
      Register::
        FieldLocation<Register::Address<Addr, Z, O, RegType, Mode>, Mask, Access, FieldType>,
      TAction>>{true, false, Addr, Mask, 0};

    template<unsigned Addr,
             unsigned Z,
             unsigned O,
             typename RegType,
             typename Mode,
             unsigned Mask,
             typename Access,
             typename FieldType,
             unsigned V>
    inline constexpr ActionWrite actionWriteOf<Register::Action<
      Register::
        FieldLocation<Register::Address<Addr, Z, O, RegType, Mode>, Mask, Access, FieldType>,
      Register::WriteLiteralAction<V>>>{true, true, Addr, Mask, V};

    namespace Detail {
        struct RowData {
            std::uint32_t base;
            std::uint32_t size;
            unsigned      id;
            bool          openAtBoot;
            ActionWrite   open;

            [[nodiscard]] constexpr bool covers(std::uint32_t addr) const {
                return addr >= base && addr - base < size;
            }
        };
    }   // namespace Detail

    /// One gated block: [Base, Base + Size) works once the gate `Id` is open, and `OpenAction` (the action type a
    /// driver's powerClockEnable writes, so the image is the same store) opens it. `OpenAtBoot`: open before
    /// Startup's phases run (out of reset, or by the chip's first init step), so writes into it claim nothing.
    template<std::uint32_t Base,
             std::uint32_t Size,
             unsigned      Id,
             typename OpenAction,
             bool OpenAtBoot = false>
    struct BlockRow {
        static constexpr std::uint32_t base       = Base;
        static constexpr std::uint32_t size       = Size;
        static constexpr unsigned      id         = Id;
        static constexpr bool          openAtBoot = OpenAtBoot;
        using Open                                = OpenAction;

        static constexpr ActionWrite open = actionWriteOf<OpenAction>;
        static_assert(
          open.literal,
          "a block row's open action is one literal field write (set/clear/write of a field)");
        static_assert(Size != 0 && Base + (Size - 1) >= Base,
                      "a block row's range is empty or wraps");

        static constexpr Detail::RowData data{Base, Size, Id, OpenAtBoot, open};
    };

    /// A chip's gated blocks. A block with two gates (two rows with one base) needs both open.
    template<typename... Rows>
    struct BlockTable {
        static constexpr std::array<Detail::RowData, sizeof...(Rows)> rows{Rows::data...};

        /// The gate `w` opens, or -1.
        static constexpr int opens(ActionWrite w) {
            if(!w.literal) { return -1; }
            for(auto const& r : rows) {
                if(r.open == w) { return static_cast<int>(r.id); }
            }
            return -1;
        }

        template<typename Action>
        static constexpr int opens() {
            return opens(actionWriteOf<Action>);
        }

        /// How many gates a write to `addr` needs (rows that cover it and are not open at boot).
        static constexpr std::size_t claimsAt(std::uint32_t addr) {
            std::size_t n = 0;
            for(auto const& r : rows) {
                if(!r.openAtBoot && r.covers(addr)) { ++n; }
            }
            return n;
        }

        /// The gate of the block at `addr` (the first row that covers it, open at boot or not), or -1.
        static constexpr int gateAt(std::uint32_t addr) {
            for(auto const& r : rows) {
                if(r.covers(addr)) { return static_cast<int>(r.id); }
            }
            return -1;
        }

        /// The rows whose block starts at `base` (2 for a two-gate block, 0 for no block).
        static constexpr std::size_t rowsOf(std::uint32_t base) {
            std::size_t n = 0;
            for(auto const& r : rows) {
                if(r.base == base) { ++n; }
            }
            return n;
        }

    private:
        template<std::uint32_t Addr, typename Row>
        using ClaimOfRow = std::conditional_t<!Row::openAtBoot && Row::data.covers(Addr),
                                              brigand::list<Gate<Row::id>>,
                                              brigand::list<>>;

        template<std::uint32_t Base, typename Row>
        using OpenOfRow = std::
          conditional_t<Row::base == Base, brigand::list<typename Row::Open>, brigand::list<>>;

        template<typename L>
        struct OneOrList {
            using type = L;
        };

        template<typename A>
        struct OneOrList<brigand::list<A>> {
            using type = A;
        };

        static constexpr bool valid() {
            for(std::size_t i = 0; i < rows.size(); ++i) {
                for(std::size_t j = i + 1; j < rows.size(); ++j) {
                    auto const& a = rows[i];
                    auto const& b = rows[j];
                    if(a.base == b.base) {
                        if(a.size != b.size || a.id == b.id) {
                            return false;
                        }   // one block, two gates
                    } else if(a.covers(b.base) || b.covers(a.base)) {
                        return false;   // two blocks overlap
                    }
                    if((a.id == b.id) != (a.open == b.open)) {
                        return false;
                    }   // one gate, one opener
                }
            }
            return true;
        }

        static_assert(
          valid(),
          "block table: rows overlap, one block lists a gate twice, or one gate has two "
          "different open actions (or two gates one)");

    public:
        /// The gates a write to `Addr` claims.
        template<std::uint32_t Addr>
        using ClaimsAt = brigand::flatten<brigand::list<ClaimOfRow<Addr, Rows>...>>;

        /// What opens the block at `Base`: one action, or a brigand::list of them for a two-gate block (an empty
        /// list for no block: the chip's front end turns that into its own error).
        template<std::uint32_t Base>
        using OpenActions =
          typename OneOrList<brigand::flatten<brigand::list<OpenOfRow<Base, Rows>...>>>::type;
    };

    /// The chip's table, a late port hook (kvasir/Port/Hooks.hpp): `using type = Hal::BlockTable<rows...>;`.
    /// Declared only: a chip without one has no gate check.
    template<typename T, typename... Ts>
    struct BlockGates;

    /// The chip's table as seen from a template over `Dep...` (the late hook resolved where Dep is complete):
    /// `typename Hal::BlockGatesOf<Regs>::type` in a chip's front end.
    template<typename... Dep>
    using BlockGatesOf = Port::LateT<BlockGates<Tag::User>, Dep...>;

    namespace Detail {
        template<typename A>
        constexpr int gateOpenedBy() {
            if constexpr(requires { typename BlockGatesOf<A>::type; }) {
                return BlockGatesOf<A>::type::opens(actionWriteOf<A>);
            } else {
                return -1;
            }
        }

        template<typename A>
        constexpr std::size_t gatesClaimedBy() {
            if constexpr(requires { typename BlockGatesOf<A>::type; }) {
                return actionWriteOf<A>.known
                       ? BlockGatesOf<A>::type::claimsAt(actionWriteOf<A>.address)
                       : 0;
            } else {
                return 0;
            }
        }
    }   // namespace Detail
}}   // namespace Kvasir::Hal

namespace Kvasir { namespace Startup {
    // The open action of a row: the block's gate is open from this phase on.
    template<typename A>
        requires(Hal::Detail::gateOpenedBy<A>() >= 0)
    struct ResourceOfAction<A> {
        using type
          = brigand::list<Hal::Gate<static_cast<unsigned>(Hal::Detail::gateOpenedBy<A>())>>;
    };

    // Any later-phase access to a register inside a gated block needs its gate(s) open.
    template<typename A>
        requires(Hal::Detail::gatesClaimedBy<A>() > 0)
    struct ClaimOfAction<A> {
        using type =
          typename Hal::BlockGatesOf<A>::type::template ClaimsAt<Hal::actionWriteOf<A>.address>;
    };
}}   // namespace Kvasir::Startup
