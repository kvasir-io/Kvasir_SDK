// Test register definitions shared by all Kvasir register tests.
#pragma once

#include "kvasir_test.hpp"

#include <cstdint>

// Register with plain unsigned fields. Bits 31..10 are write-ignored-if-zero (reserved), so writes
// covering dat/cmd/stop never need a read-modify-write for the upper bits; a read-modify-write writes
// them back as read (Semantics::writeBackActs 0, the default).
struct SimpleTestReg {
    using Addr = Kvasir::Register::Address<0x10, 0xFFFFFC00, 0x00000000, std::uint32_t>;

    static constexpr Kvasir::Test::Field<Addr, 9, 9> stop{};

    static constexpr Kvasir::Test::Field<Addr, 8, 8> cmd{};

    static constexpr Kvasir::Test::Field<Addr, 7, 0> dat{};

    using default_values
      = decltype(Kvasir::MPL::list(write(stop, Kvasir::Register::value<std::uint32_t, 0>()),
                                   write(cmd, Kvasir::Register::value<std::uint32_t, 0>()),
                                   write(dat, Kvasir::Register::value<std::uint32_t, 0>())));

    template<typename... Actions>
    [[gnu::always_inline]] static constexpr auto overrideDefaults(Actions const&... actions) {
        return Kvasir::Register::overrideDefaults<default_values>(actions...);
    }
};

// One field per access kind, for the access traits the factories assert.
struct AccessTestReg {
    using Addr = Kvasir::Register::Address<0x80, 0x00000000, 0x00000000, std::uint32_t>;

    static constexpr Kvasir::Test::Field<Addr, 0, 0, Kvasir::Register::ReadOnlyAccess> ro{};

    static constexpr Kvasir::Test::Field<Addr, 1, 1, Kvasir::Register::WriteOnlyAccess> wo{};

    static constexpr Kvasir::Test::Field<Addr, 2, 2> rw{};

    static constexpr Kvasir::Test::
      Field<Addr, 3, 3, Kvasir::Register::Access<Kvasir::Register::AccessType::writeOnce>>
        wonce{};

    static constexpr Kvasir::Test::
      Field<Addr, 4, 4, Kvasir::Register::Access<Kvasir::Register::AccessType::readWriteOnce>>
        rwonce{};
};

// Same layout as SimpleTestReg but with enum class typed fields and FieldValue constants.
struct ComplexTestReg {
    using Addr = Kvasir::Register::Address<0x10, 0xFFFFFC00, 0x00000000, std::uint32_t>;

    enum class STOPVal : std::uint32_t {
        disable = 0,
        enable  = 1,
    };

    static constexpr Kvasir::Test::Field<Addr, 9, 9, Kvasir::Register::ReadWriteAccess, STOPVal>
      stop{};

    struct STOPValC {
        static constexpr Kvasir::Register::FieldValue<typename decltype(stop)::type,
                                                      STOPVal::disable>
          disable{};
        static constexpr Kvasir::Register::FieldValue<typename decltype(stop)::type,
                                                      STOPVal::enable>
          enable{};
    };

    enum class CMDVal : std::uint32_t {
        write = 0,
        read  = 1,
    };

    static constexpr Kvasir::Test::Field<Addr, 8, 8, Kvasir::Register::ReadWriteAccess, CMDVal>
      cmd{};

    struct CMDValC {
        static constexpr Kvasir::Register::FieldValue<typename decltype(cmd)::type, CMDVal::write>
          write{};
        static constexpr Kvasir::Register::FieldValue<typename decltype(cmd)::type, CMDVal::read>
          read{};
    };

    static constexpr Kvasir::Test::Field<Addr, 7, 0> dat{};

    using default_values
      = decltype(Kvasir::MPL::list(write(STOPValC::disable),
                                   write(CMDValC::write),
                                   write(dat, Kvasir::Register::value<std::uint32_t, 0>())));

    template<typename... Actions>
    [[gnu::always_inline]] static constexpr auto overrideDefaults(Actions const&... actions) {
        return Kvasir::Register::overrideDefaults<default_values>(actions...);
    }
};

// Second register at a different address for multi-register tests.
struct SecondReg {
    using Addr = Kvasir::Register::Address<0x20, 0xFFFFFF00, 0x00000000, std::uint32_t>;

    static constexpr Kvasir::Test::Field<Addr, 7, 0> data{};

    static constexpr Kvasir::Test::Field<Addr, 15, 8> status{};
};

// Third register at yet another address for 3-register tests.
struct ThirdReg {
    using Addr = Kvasir::Register::Address<0x30, 0xFFFF0000, 0x00000000, std::uint32_t>;

    static constexpr Kvasir::Test::Field<Addr, 7, 0> control{};

    static constexpr Kvasir::Test::Field<Addr, 15, 8> config{};

    static constexpr Kvasir::Test::Field<Addr, 23, 16> mode{};
};

// Register without any write-ignored masks: every partial write needs read-modify-write.
// Also carries special access modes for the set/clear/reset factory tests.
struct CtrlReg {
    using Addr = Kvasir::Register::Address<0x50, 0x00000000, 0x00000000, std::uint32_t>;

    static constexpr Kvasir::Test::Field<Addr, 0, 0> en{};

    static constexpr Kvasir::Test::Field<Addr, 1, 1> irq{};

    // write-one-to-clear flag bit
    static constexpr Kvasir::Test::Field<
      Addr,
      3,
      3,
      Kvasir::Register::Access<Kvasir::Register::AccessType::readWrite,
                               Kvasir::Register::ReadActionType::normal,
                               Kvasir::Register::ModifiedWriteValueType::oneToClear>>
      flag{};

    static constexpr Kvasir::Test::Field<Addr, 11, 4> div{};
};

// GPIO-style toggle register: the hardware flips a bit for every written one, writing
// zero is a no-op - the whole register is therefore write-ignored-if-zero, and a one read
// back would toggle again: every bit is written 0 by a read-modify-write (writeBackActs; this
// is also how svd_converter generates oneToToggle fields).
struct ToggleReg {
    using Addr = Kvasir::Register::Address<0x70,
                                           0xFFFFFFFF,
                                           0x00000000,
                                           std::uint32_t,
                                           Kvasir::Register::Mode<{.writeBackActs = 0xFFFFFFFFU}>>;

    using ToggleAccess
      = Kvasir::Register::Access<Kvasir::Register::AccessType::readWrite,
                                 Kvasir::Register::ReadActionType::normal,
                                 Kvasir::Register::ModifiedWriteValueType::oneToToggle>;

    static constexpr Kvasir::Test::Field<Addr, 5, 5, ToggleAccess> pin5{};

    static constexpr Kvasir::Test::Field<Addr, 6, 6, ToggleAccess> pin6{};
};

// toggle bit surrounded by plain read-write bits in a register without any
// write-ignored masks
struct PlainToggleReg {
    using Addr = Kvasir::Register::Address<0xF0, 0x00000000, 0x00000000, std::uint32_t>;

    static constexpr Kvasir::Test::Field<Addr, 5, 5, ToggleReg::ToggleAccess> tgl{};

    static constexpr Kvasir::Test::Field<Addr, 1, 0> mode{};
};

// Register with both write-ignored masks: bits 31..8 are write-ignored-if-zero and
// bits 7..4 are write-ignored-if-one, only bits 3..0 behave like normal RW bits. Of bits
// 31..8 only the w1c flag done (bit 8) is written 0 by a read-modify-write (writeBackActs, as
// svd_converter emits it); the reserved rest goes back as read.
struct MaskedReg {
    using Addr = Kvasir::Register::Address<0x60,
                                           0xFFFFFF00,
                                           0x000000F0,
                                           std::uint32_t,
                                           Kvasir::Register::Mode<{.writeBackActs = 0x00000100U}>>;

    static constexpr Kvasir::Test::Field<Addr, 1, 0> low{};

    static constexpr Kvasir::Test::Field<Addr, 3, 2> high{};

    // write-one-to-clear flags inside the write-ignored-if-zero area
    static constexpr Kvasir::Test::Field<
      Addr,
      8,
      8,
      Kvasir::Register::Access<Kvasir::Register::AccessType::readWrite,
                               Kvasir::Register::ReadActionType::normal,
                               Kvasir::Register::ModifiedWriteValueType::oneToClear>>
      done{};
};

// Status register for the register model tests: two plain status bits and a full field
// spanning the register.
struct StatusReg {
    using Addr = Kvasir::Register::Address<0x90, 0x00000000, 0x00000000, std::uint32_t>;

    static constexpr Kvasir::Test::Field<Addr, 0, 0, Kvasir::Register::ReadOnlyAccess> ready{};

    static constexpr Kvasir::Test::Field<Addr, 2, 2, Kvasir::Register::ReadOnlyAccess> overflow{};

    static constexpr Kvasir::Test::Field<Addr, 31, 0> all{};
};

// FIFO data register for the register model tests: a read pops, a write pushes. data spans
// the register, so writing it needs no read; low is a partial field, whose write is a
// read-modify-write - and that read pops the FIFO.
struct DataReg {
    using Addr = Kvasir::Register::Address<0xA0, 0x00000000, 0x00000000, std::uint32_t>;

    static constexpr Kvasir::Test::Field<Addr, 31, 0> data{};

    static constexpr Kvasir::Test::Field<Addr, 7, 0> low{};
};

// A two-bit write-one-to-clear flag field beside a plain one, for reset() on a multi-bit field.
struct FlagPairReg {
    using Addr = Kvasir::Register::Address<0xB0, 0x00000000, 0x00000000, std::uint32_t>;

    static constexpr Kvasir::Test::Field<Addr, 1, 0, Kvasir::Register::ROneToClearAccess> pair{};

    static constexpr Kvasir::Test::Field<Addr, 7, 4> mode{};
};

// A key register, AIRCR-like: bits 31..16 must be written with the key and read back as something
// else, so a partial write that does not name them may not read-modify-write.
struct KeyReg {
    using Addr = Kvasir::Register::Address<0xC0,
                                           0x00000000,
                                           0x00000000,
                                           std::uint32_t,
                                           Kvasir::Register::Mode<{.mustSupply = 0xFFFF0000U}>>;

    static constexpr Kvasir::Test::Field<Addr, 31, 16> key{};

    static constexpr Kvasir::Test::Field<Addr, 2, 2> reset{};
};

// A register with no readable field (svd_converter --write-only-registers=derived), BSRR-like: a partial write
// never reads it and writes the bits it does not name as 0.
struct SetResetReg {
    using Addr = Kvasir::Register::Address<0xE0,
                                           0x00000000,
                                           0x00000000,
                                           std::uint32_t,
                                           Kvasir::Register::Mode<{.writeOnlyRegister = true}>>;

    static constexpr Kvasir::Test::Field<Addr, 5, 5, Kvasir::Register::WriteOnlyAccess> bs5{};

    static constexpr Kvasir::Test::Field<Addr, 21, 21, Kvasir::Register::WriteOnlyAccess> br5{};
};

// A control register with an unclassified write-only bit 11 (like FLASH_ACR.ICRST), with --write-only-mask: a partial
// write must name it, like a key.
struct IcrstReg {
    using Addr = Kvasir::Register::Address<0xE4,
                                           0x00000000,
                                           0x00000000,
                                           std::uint32_t,
                                           Kvasir::Register::Mode<{.writeOnlyNoIdentity = 0x800}>>;

    static constexpr Kvasir::Test::Field<Addr, 3, 0> latency{};

    static constexpr Kvasir::Test::Field<Addr, 11, 11, Kvasir::Register::WriteOnlyAccess> icrst{};
};

// A FIFO data register whose read pops: never read-modify-written.
struct FifoReg {
    using Addr = Kvasir::Register::Address<0xD0,
                                           0x00000000,
                                           0x00000000,
                                           std::uint32_t,
                                           Kvasir::Register::Mode<{.readSideEffect = true}>>;

    static constexpr Kvasir::Test::Field<Addr, 31, 0> all{};

    static constexpr Kvasir::Test::Field<Addr, 7, 0> data{};
};

// Whole registers as svd_converter makes their FULLREGISTER: a FIFO push register, none of whose fields can be read
// (FULLREGISTER write-only: readWhole / read() of it is a compile error), and a field-less read-only ID register
// (writeWhole of it is one).
struct WholeWriteOnlyReg {
    using Addr = Kvasir::Register::Address<0xE8,
                                           0x00000000,
                                           0x00000000,
                                           std::uint32_t,
                                           Kvasir::Register::Mode<{.writeOnlyRegister = true}>>;

    static constexpr Kvasir::Test::Field<Addr, 31, 0, Kvasir::Register::WriteOnlyAccess>
      FULLREGISTER{};
};

struct WholeReadOnlyReg {
    using Addr = Kvasir::Register::Address<0xEC, 0x00000000, 0x00000000, std::uint32_t>;

    static constexpr Kvasir::Test::Field<Addr, 31, 0, Kvasir::Register::ReadOnlyAccess>
      FULLREGISTER{};
};

// A register with a reset value, for Register::fromReset<R>: a write is one store, the bits it does not name at
// resetValue (here bit 1 and bits 15:12 are 1 out of reset).
struct ResetReg {
    using Addr = Kvasir::Register::Address<0x58, 0x00000000, 0x00000000, std::uint32_t>;

    static constexpr std::uint32_t resetValue = 0x0000F002;

    static constexpr Kvasir::Test::Field<Addr, 0, 0> en{};

    static constexpr Kvasir::Test::Field<Addr, 1, 1> irq{};

    static constexpr Kvasir::Test::Field<Addr, 11, 4> div{};
};

// Every shared register sits at an address of its own (two registers at one address share the recorder's read queue
// and model), except ComplexTestReg: SimpleTestReg's twin at 0x10 by design, the templated tests run one body on both.
// A test's own registers live at 0x100 and up and are checked against these: static_assert(distinctFromShared<...>).
template<typename... Local>
inline constexpr bool distinctFromShared = Kvasir::Test::distinctAddresses<SimpleTestReg,
                                                                           AccessTestReg,
                                                                           SecondReg,
                                                                           ThirdReg,
                                                                           CtrlReg,
                                                                           ToggleReg,
                                                                           PlainToggleReg,
                                                                           MaskedReg,
                                                                           StatusReg,
                                                                           DataReg,
                                                                           FlagPairReg,
                                                                           KeyReg,
                                                                           SetResetReg,
                                                                           IcrstReg,
                                                                           FifoReg,
                                                                           WholeWriteOnlyReg,
                                                                           WholeReadOnlyReg,
                                                                           ResetReg,
                                                                           Local...>();
static_assert(distinctFromShared<>);
