// readWhole / writeWhole / writeLane / Array (Register.hpp): one plain load or store of the whole register, of one lane
// of it, or of a register array's element picked at run time. No read-modify-write, no key added, the store's width is
// the lane's.
#include "test_registers.hpp"

#include <cstdint>
#include <print>
#include <type_traits>

using namespace Kvasir::Register;
using namespace Kvasir::Test;

namespace {
// a register whose upper bits are a key (as SCB AIRCR): writeWhole writes the word as given, nothing added
struct KeyedReg {
    using Addr
      = Address<0x120, 0x00000000, 0x00000000, std::uint32_t, Mode<{.mustSupply = 0xFFFF0000U}>>;

    static constexpr FieldLocation<Addr, maskFromRange(31, 0), ReadWriteAccess, std::uint32_t>
      FULLREGISTER{};
};

struct ReadOnlyReg {
    using Addr = Address<0x124, 0x00000000, 0x00000000, std::uint32_t>;

    static constexpr FieldLocation<Addr, maskFromRange(31, 0), ReadOnlyAccess, std::uint32_t>
      FULLREGISTER{};
};

static_assert(std::is_same_v<decltype(readWhole(KeyedReg::FULLREGISTER)),
                             std::uint32_t>);

void readsOnce() {
    test("readWhole: one read of the register");
    recorder.setReadValues(ReadOnlyReg::Addr::value, {0x1234'5678U});
    CHECK(readWhole(ReadOnlyReg::FULLREGISTER) == 0x1234'5678U);
    checkActions({
      R{ReadOnlyReg::Addr::value, 0x1234'5678U}
    });
}

void writesAsGiven() {
    test("writeWhole: one write, the word as given, no read first");
    auto const v = runtimeValue(0x05FA'0004U);
    writeWhole(KeyedReg::FULLREGISTER, v);
    checkActions({
      W{KeyedReg::Addr::value, 0x05FA'0004U}
    });
}

void wholeWriteOnlyAndReadOnly() {
    test("writeWhole of a write-only FULLREGISTER, readWhole of a read-only one: one access each");
    recorder.setReadValues(WholeReadOnlyReg::Addr::value, {0x0BADC0DEU});
    writeWhole(WholeWriteOnlyReg::FULLREGISTER, runtimeValue(0x55U));
    CHECK(readWhole(WholeReadOnlyReg::FULLREGISTER) == 0x0BADC0DEU);
    checkActions({
      W{WholeWriteOnlyReg::Addr::value,       0x55U},
      R{ WholeReadOnlyReg::Addr::value, 0x0BADC0DEU}
    });
}

void writesOneLane() {
    test("writeLane: a byte store at the lane's address");
    writeLane<std::uint8_t, 0>(KeyedReg::FULLREGISTER, std::uint8_t{3});
    writeLane<std::uint8_t, 2>(KeyedReg::FULLREGISTER, std::uint8_t{0x7F});
    writeLane<std::uint16_t, 1>(KeyedReg::FULLREGISTER, std::uint16_t{0xBEEF});
    checkActions({
      W{    KeyedReg::Addr::value,      3U},
      W{KeyedReg::Addr::value + 2,   0x7FU},
      W{KeyedReg::Addr::value + 2, 0xBEEFU}
    });
}

// a register array as the converter writes one (PIO INSTR_MEM<n>): 4 elements, 8 bytes apart
template<unsigned Index>
struct ArrayReg {
    static_assert(Index < 4);
    using Addr = Address<0x100 + (Index * 0x08), 0x00000000, 0x00000000, std::uint32_t>;

    static constexpr FieldLocation<Addr, maskFromRange(31, 0), ReadWriteAccess, std::uint32_t>
      FULLREGISTER{};
};

static_assert(extract(SimpleTestReg::cmd,
                      0x0000'0300U)
                  == 1U
                && extract(SimpleTestReg::dat,
                           0x0000'03A5U)
                     == 0xA5U,
              "extract: a field of a whole-register word");
static_assert(deposit(SimpleTestReg::cmd,
                      1U)
                  == 0x100U
                && deposit(SimpleTestReg::dat,
                           0x1A5U)
                     == 0xA5U,
              "deposit: a field's bits, cut to the field");

static_assert(extract(SimpleTestReg::cmd,
                      0x0000'0100U)
                  == 1U
                && extract(SimpleTestReg::cmd,
                           0xFFFF'FEFFU)
                     == 0U,
              "extract of a one-bit field: 0 or 1");

// bit<N>: one bit of a multi-bit field, a one-bit field of the same register and access. SM_ENABLE 3:0 read-write,
// RESTART 7:4 write-one-to-set (writes of 0 ignored), everything else reserved.
struct SmReg {
    using Addr = Address<0x128,
                         0xFFFFFFF0U,
                         0x00000000,
                         std::uint32_t,
                         Mode<{.writeBackActs = 0x000000F0U}>>;
    static constexpr FieldLocation<Addr, maskFromRange(3, 0), ReadWriteAccess, std::uint32_t>
      smEnable{};
    static constexpr FieldLocation<
      Addr,
      maskFromRange(7, 4),
      Access<AccessType::readWrite, ReadActionType::normal, ModifiedWriteValueType::oneToSet>,
      std::uint32_t>
      restart{};
};

static_assert(decltype(bit<2>(SmReg::smEnable))::mask == 0x4U
              && decltype(bit<3>(SmReg::restart))::mask == 0x80U);
static_assert(std::is_same_v<decltype(bit<1>(SmReg::restart))::Access,
                             decltype(SmReg::restart)::Access>,
              "bit<N> keeps the field's write semantics");

void bitOfAField() {
    test("bit<N>: set/clear one bit of a field; a one-to-set bit is never written back");
    constexpr unsigned v
      = 0xAB00'00F9U;   // reserved bits set, a restart bit reads 1, SM_ENABLE 0b1001
    recorder.setReadValues(SmReg::Addr::value, {v, v, v, v});
    apply(set(bit<1>(SmReg::smEnable)));
    apply(clear(bit<3>(SmReg::smEnable)));
    apply(
      set(bit<2>(SmReg::restart)));   // keeps SM_ENABLE: a read, the other restart bits written 0
    CHECK(get<0>(apply(read(bit<0>(SmReg::smEnable)))) == 1U);
    checkActions({
      R{SmReg::Addr::value,                    v},
      W{SmReg::Addr::value,  (v & ~0xF0U) | 0x2U},
      R{SmReg::Addr::value,                    v},
      W{SmReg::Addr::value,   v & ~0xF0U & ~0x8U},
      R{SmReg::Addr::value,                    v},
      W{SmReg::Addr::value, (v & ~0xF0U) | 0x40U},
      R{SmReg::Addr::value,                    v}
    });
}

using Arr = Kvasir::Register::Array<ArrayReg, 4>;
static_assert(Arr::base == 0x100 && Arr::stride == 8 && Arr::size == 4);

void arrayIndexedAtRunTime() {
    test("Array: element i at base + i x stride, one access each");
    recorder.setReadValues(0x118, {0xAB});
    for(unsigned i = 0; i < Arr::size; ++i) { Arr::writeWhole(i, 0x10 + i); }
    CHECK(Arr::readWhole(runtimeValue(3)) == 0xABU);
    checkActions({
      W{0x100, 0x10},
      W{0x108, 0x11},
      W{0x110, 0x12},
      W{0x118, 0x13},
      R{0x118, 0xAB}
    });
}

// RegisterTraits: the same facts from an Address, a FieldLocation of it and an Action on it
struct HalfReg {
    using Addr = Address<0x12C, 0x0000FF00, 0x00000000, std::uint16_t>;

    static constexpr FieldLocation<Addr, maskFromRange(7, 0), ReadWriteAccess, std::uint8_t> low{};
};

using HalfField = std::remove_cvref_t<decltype(HalfReg::low)>;
static_assert(distinctFromShared<KeyedReg,
                                 ReadOnlyReg,
                                 ArrayReg<0>,
                                 ArrayReg<1>,
                                 ArrayReg<2>,
                                 ArrayReg<3>,
                                 SmReg,
                                 HalfReg>);
static_assert(RegisterTraits<HalfReg::Addr>::address == 0x12CU);
static_assert(RegisterTraits<HalfField>::address == 0x12CU);
static_assert(RegisterTraits<decltype(read(HalfReg::low))>::address == 0x12CU);
static_assert(std::is_same_v<RegisterTraits<HalfReg::Addr>::RegType,
                             std::uint16_t>);
static_assert(std::is_same_v<RegisterTraits<HalfField>::RegType,
                             std::uint16_t>);
static_assert(std::is_same_v<RegisterTraits<decltype(read(HalfReg::low))>::RegType,
                             std::uint16_t>);
static_assert(RegisterTraits<HalfField>::writeIgnoredIfZeroMask == 0x0000FF00U);
static_assert(RegisterTraits<HalfField>::allBitsSetMask == 0xFFFFU);
static_assert(std::is_same_v<decltype(RegisterTraits<HalfField>::read()),
                             std::uint16_t>);

// a register picked at run time: fold over the candidates, read the match, one read of it
template<typename... Regs>
std::uint32_t readPicked(unsigned which) {
    std::uint32_t word = 0;
    unsigned      i    = 0;
    (void)((i++ == which && (word = RegisterTraits<typename Regs::Addr>::read(), true)) || ...);
    return word;
}

void registerPickedAtRunTime() {
    test("RegisterTraits: a register picked at run time is one read of it");
    recorder.setReadValues(HalfReg::Addr::value, {0xBEEFU});
    CHECK(readPicked<ReadOnlyReg, HalfReg>(runtimeValue(1U)) == 0xBEEFU);
    checkActions({
      R{HalfReg::Addr::value, 0xBEEFU}
    });
}
}   // namespace

int main() {
    readsOnce();
    writesAsGiven();
    wholeWriteOnlyAndReadOnly();
    writesOneLane();
    arrayIndexedAtRunTime();
    bitOfAField();
    registerPickedAtRunTime();

    return Kvasir::Test::report();
}
