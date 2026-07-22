// Test register definitions shared by all Kvasir register tests.
#pragma once

#include "kvasir_test.hpp"

#include <cstdint>

// Register with plain unsigned fields. Bits 31..10 are write-ignored-if-zero, so writes
// covering dat/cmd/stop never need a read-modify-write for the upper bits.
struct SimpleTestReg {
    using Addr = Kvasir::Register::Address<0x10, 0xFFFFFC00, 0x00000000, std::uint32_t>;

    static constexpr Kvasir::Register::FieldLocation<Addr,
                                                     Kvasir::Register::maskFromRange(9, 9),
                                                     Kvasir::Register::WriteOnlyAccess,
                                                     std::uint32_t>
      stop{};

    static constexpr Kvasir::Register::FieldLocation<Addr,
                                                     Kvasir::Register::maskFromRange(8, 8),
                                                     Kvasir::Register::WriteOnlyAccess,
                                                     std::uint32_t>
      cmd{};

    static constexpr Kvasir::Register::FieldLocation<Addr,
                                                     Kvasir::Register::maskFromRange(7, 0),
                                                     Kvasir::Register::ReadWriteAccess,
                                                     std::uint32_t>
      dat{};

    using default_values
      = decltype(Kvasir::MPL::list(write(stop, Kvasir::Register::value<std::uint32_t, 0>()),
                                   write(cmd, Kvasir::Register::value<std::uint32_t, 0>()),
                                   write(dat, Kvasir::Register::value<std::uint32_t, 0>())));

    template<typename... Actions>
    static constexpr auto overrideDefaultsRuntime(Actions const&... actions) {
        return Kvasir::Register::overrideDefaultsRuntime<default_values>::exec(actions...);
    }
};

// Same layout as SimpleTestReg but with enum class typed fields and FieldValue constants.
struct ComplexTestReg {
    using Addr = Kvasir::Register::Address<0x10, 0xFFFFFC00, 0x00000000, std::uint32_t>;

    enum class STOPVal : std::uint32_t {
        disable = 0,
        enable  = 1,
    };

    static constexpr Kvasir::Register::FieldLocation<Addr,
                                                     Kvasir::Register::maskFromRange(9, 9),
                                                     Kvasir::Register::WriteOnlyAccess,
                                                     STOPVal>
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

    static constexpr Kvasir::Register::FieldLocation<Addr,
                                                     Kvasir::Register::maskFromRange(8, 8),
                                                     Kvasir::Register::WriteOnlyAccess,
                                                     CMDVal>
      cmd{};

    struct CMDValC {
        static constexpr Kvasir::Register::FieldValue<typename decltype(cmd)::type, CMDVal::write>
          write{};
        static constexpr Kvasir::Register::FieldValue<typename decltype(cmd)::type, CMDVal::read>
          read{};
    };

    static constexpr Kvasir::Register::FieldLocation<Addr,
                                                     Kvasir::Register::maskFromRange(7, 0),
                                                     Kvasir::Register::ReadWriteAccess,
                                                     std::uint32_t>
      dat{};

    using default_values
      = decltype(Kvasir::MPL::list(write(STOPValC::disable),
                                   write(CMDValC::write),
                                   write(dat, Kvasir::Register::value<std::uint32_t, 0>())));

    template<typename... Actions>
    static constexpr auto overrideDefaultsRuntime(Actions const&... actions) {
        return Kvasir::Register::overrideDefaultsRuntime<default_values>::exec(actions...);
    }
};

// Second register at a different address for multi-register tests.
struct SecondReg {
    using Addr = Kvasir::Register::Address<0x20, 0xFFFFFF00, 0x00000000, std::uint32_t>;

    static constexpr Kvasir::Register::FieldLocation<Addr,
                                                     Kvasir::Register::maskFromRange(7, 0),
                                                     Kvasir::Register::ReadWriteAccess,
                                                     std::uint32_t>
      data{};

    static constexpr Kvasir::Register::FieldLocation<Addr,
                                                     Kvasir::Register::maskFromRange(15, 8),
                                                     Kvasir::Register::ReadWriteAccess,
                                                     std::uint32_t>
      status{};
};

// Third register at yet another address for 3-register tests.
struct ThirdReg {
    using Addr = Kvasir::Register::Address<0x30, 0xFFFF0000, 0x00000000, std::uint32_t>;

    static constexpr Kvasir::Register::FieldLocation<Addr,
                                                     Kvasir::Register::maskFromRange(7, 0),
                                                     Kvasir::Register::ReadWriteAccess,
                                                     std::uint32_t>
      control{};

    static constexpr Kvasir::Register::FieldLocation<Addr,
                                                     Kvasir::Register::maskFromRange(15, 8),
                                                     Kvasir::Register::ReadWriteAccess,
                                                     std::uint32_t>
      config{};

    static constexpr Kvasir::Register::FieldLocation<Addr,
                                                     Kvasir::Register::maskFromRange(23, 16),
                                                     Kvasir::Register::ReadWriteAccess,
                                                     std::uint32_t>
      mode{};
};

// Register without any write-ignored masks: every partial write needs read-modify-write.
// Also carries special access modes for the set/clear/reset factory tests.
struct CtrlReg {
    using Addr = Kvasir::Register::Address<0x50, 0x00000000, 0x00000000, std::uint32_t>;

    static constexpr Kvasir::Register::FieldLocation<Addr,
                                                     Kvasir::Register::maskFromRange(0, 0),
                                                     Kvasir::Register::ReadWriteAccess,
                                                     std::uint32_t>
      en{};

    static constexpr Kvasir::Register::FieldLocation<Addr,
                                                     Kvasir::Register::maskFromRange(1, 1),
                                                     Kvasir::Register::ReadWriteAccess,
                                                     std::uint32_t>
      irq{};

    // write-one-to-clear flag bit
    static constexpr Kvasir::Register::FieldLocation<
      Addr,
      Kvasir::Register::maskFromRange(3, 3),
      Kvasir::Register::Access<Kvasir::Register::AccessType::readWrite,
                               Kvasir::Register::ReadActionType::normal,
                               Kvasir::Register::ModifiedWriteValueType::oneToClear>,
      std::uint32_t>
      flag{};

    static constexpr Kvasir::Register::FieldLocation<Addr,
                                                     Kvasir::Register::maskFromRange(11, 4),
                                                     Kvasir::Register::ReadWriteAccess,
                                                     std::uint32_t>
      div{};
};

// GPIO-style toggle register: the hardware flips a bit for every written one, writing
// zero is a no-op - the whole register is therefore write-ignored-if-zero (this is
// also how svd_converter generates oneToToggle fields).
struct ToggleReg {
    using Addr = Kvasir::Register::Address<0x70, 0xFFFFFFFF, 0x00000000, std::uint32_t>;

    using ToggleAccess
      = Kvasir::Register::Access<Kvasir::Register::AccessType::readWrite,
                                 Kvasir::Register::ReadActionType::normal,
                                 Kvasir::Register::ModifiedWriteValueType::oneToToggle>;

    static constexpr Kvasir::Register::
      FieldLocation<Addr, Kvasir::Register::maskFromRange(5, 5), ToggleAccess, std::uint32_t>
        pin5{};

    static constexpr Kvasir::Register::
      FieldLocation<Addr, Kvasir::Register::maskFromRange(6, 6), ToggleAccess, std::uint32_t>
        pin6{};
};

// toggle bit surrounded by plain read-write bits in a register without any
// write-ignored masks
struct PlainToggleReg {
    using Addr = Kvasir::Register::Address<0x80, 0x00000000, 0x00000000, std::uint32_t>;

    static constexpr Kvasir::Register::FieldLocation<Addr,
                                                     Kvasir::Register::maskFromRange(5, 5),
                                                     ToggleReg::ToggleAccess,
                                                     std::uint32_t>
      tgl{};

    static constexpr Kvasir::Register::FieldLocation<Addr,
                                                     Kvasir::Register::maskFromRange(1, 0),
                                                     Kvasir::Register::ReadWriteAccess,
                                                     std::uint32_t>
      mode{};
};

// Register with both write-ignored masks: bits 31..8 are write-ignored-if-zero and
// bits 7..4 are write-ignored-if-one, only bits 3..0 behave like normal RW bits.
struct MaskedReg {
    using Addr = Kvasir::Register::Address<0x60, 0xFFFFFF00, 0x000000F0, std::uint32_t>;

    static constexpr Kvasir::Register::FieldLocation<Addr,
                                                     Kvasir::Register::maskFromRange(1, 0),
                                                     Kvasir::Register::ReadWriteAccess,
                                                     std::uint32_t>
      low{};

    static constexpr Kvasir::Register::FieldLocation<Addr,
                                                     Kvasir::Register::maskFromRange(3, 2),
                                                     Kvasir::Register::ReadWriteAccess,
                                                     std::uint32_t>
      high{};

    // write-one-to-clear flags inside the write-ignored-if-zero area
    static constexpr Kvasir::Register::FieldLocation<
      Addr,
      Kvasir::Register::maskFromRange(8, 8),
      Kvasir::Register::Access<Kvasir::Register::AccessType::readWrite,
                               Kvasir::Register::ReadActionType::normal,
                               Kvasir::Register::ModifiedWriteValueType::oneToClear>,
      std::uint32_t>
      done{};
};
