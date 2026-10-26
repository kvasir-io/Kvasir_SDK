// Register::Semantics::writeBackActs (Types.hpp, emitted by svd_converter): a read-modify-write writes only the one-to-* bits of
// the write-ignored-if-zero mask as 0; reserved and read-only bits go back as read. A set() is read, OR, write; a
// clear() clears the field's own bits; a pending write-one-to-clear flag is never written back as 1.
#include "test_registers.hpp"

#include <cstdint>
#include <print>

using namespace Kvasir::Register;
using namespace Kvasir::Test;

namespace {
using W1cAccess
  = Access<AccessType::readWrite, ReadActionType::normal, ModifiedWriteValueType::oneToClear>;

// flags 3:0 write-one-to-clear, en 8, mode 15:12, a read-only bit 20, everything else reserved
template<typename TMode>
struct FlagReg {
    using Addr = Address<0x300, 0xFFFF0EFFU, 0x00000000, std::uint32_t, TMode>;

    static constexpr FieldLocation<Addr, maskFromRange(3, 0), W1cAccess, std::uint32_t> flags{};
    static constexpr FieldLocation<Addr, maskFromRange(8, 8), ReadWriteAccess, std::uint32_t> en{};
    static constexpr FieldLocation<Addr, maskFromRange(15, 12), ReadWriteAccess, std::uint32_t>
      mode{};
    static constexpr FieldLocation<Addr, maskFromRange(20, 20), ReadOnlyAccess, std::uint32_t> ro{};
};

using New     = FlagReg<Mode<{.writeBackActs = 0x0000000FU}>>;
using Default = FlagReg<Mode<>>;
using Zeroes  = FlagReg<Mode<{.writeBackActs = 0xFFFF0EFFU}>>;

// reserved bits 31:24 and the read-only bit read 1 here, two flags are pending, mode is 5
constexpr unsigned readBack = 0xAB10'5106U;

static_assert(RegisterTraits<New::Addr>::writeBackActsMask == 0x0000000FU);
static_assert(RegisterTraits<Default::Addr>::writeBackActsMask == 0U,
              "a register without writeBackActs writes every bit back as read");
static_assert(RegisterTraits<Zeroes::Addr>::writeBackActsMask == 0xFFFF0EFFU);

void setKeepsReservedAndReadOnly() {
    test("set(): reserved and read-only bits as read, pending flags written 0");
    recorder.setReadValue(New::Addr::value, readBack);
    apply(set(New::en));
    checkActions({
      R{New::Addr::value,                       readBack},
      W{New::Addr::value, (readBack & ~0xFU) | (1U << 8)}
    });
}

void clearTouchesOnlyTheField() {
    test("clear(): the field's own bits, nothing else");
    recorder.setReadValue(New::Addr::value, readBack);
    apply(clear(New::en));
    checkActions({
      R{New::Addr::value,                      readBack},
      W{New::Addr::value, readBack & ~0xFU & ~(1U << 8)}
    });
}

void literalWrite() {
    test("write(field, literal): the field's zeros cleared, its ones set");
    recorder.setReadValue(New::Addr::value, readBack);
    apply(write(New::mode, value<std::uint32_t, 0xAU>()));
    checkActions({
      R{New::Addr::value,                                readBack},
      W{New::Addr::value, (readBack & ~0xFU & ~0xF000U) | 0xA000U}
    });
}

void runtimeWrite() {
    test("write(field, runtime): the whole field cleared first");
    recorder.setReadValue(New::Addr::value, readBack);
    apply(write(New::mode, runtimeValue(0x3U)));
    checkActions({
      R{New::Addr::value,                                readBack},
      W{New::Addr::value, (readBack & ~0xFU & ~0xF000U) | 0x3000U}
    });
}

void flagClearWritesOnlyThatFlag() {
    test("write() of one flag: that flag 1, the other pending flag 0");
    recorder.setReadValue(New::Addr::value, readBack);
    apply(write(New::flags, value<std::uint32_t, 0x2U>()));
    checkActions({
      R{New::Addr::value,                  readBack},
      W{New::Addr::value, (readBack & ~0xFU) | 0x2U}
    });
}

void defaultWritesBackAsRead() {
    test(
      "without writeBackActs: every bit written back as read - the pending flags too (state them)");
    recorder.setReadValue(Default::Addr::value, readBack);
    apply(set(Default::en));
    checkActions({
      R{Default::Addr::value,             readBack},
      W{Default::Addr::value, readBack | (1U << 8)}
    });
}

void statedMaskZeroesItsBits() {
    test(
      "writeBackActs = the whole write-ignored-if-zero mask: all of it written 0 (SAM PINCFG's "
      "reserved bits)");
    recorder.setReadValue(Zeroes::Addr::value, readBack);
    apply(set(Zeroes::en));
    checkActions({
      R{Zeroes::Addr::value,                              readBack},
      W{Zeroes::Addr::value, (readBack & ~0xFFFF0EFFU) | (1U << 8)}
    });
}
}   // namespace

int main() {
    setKeepsReservedAndReadOnly();
    clearTouchesOnlyTheField();
    literalWrite();
    runtimeWrite();
    flagClearWritesOnlyThatFlag();
    defaultWritesBackAsRead();
    statedMaskZeroesItsBits();

    return Kvasir::Test::report();
}
