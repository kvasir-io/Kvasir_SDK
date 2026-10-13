// Register::WriteBackActs (Types.hpp, emitted by svd_converter): a read-modify-write writes only the one-to-* bits of
// the write-ignored-if-zero mask as 0; reserved and read-only bits go back as read. A set() is read, OR, write; a
// clear() clears the field's own bits; a pending write-one-to-clear flag is never written back as 1.
#include "test_registers.hpp"

#include <cstdint>
#include <print>

using namespace Kvasir::Register;
using namespace Kvasir::Test;
using R = Recorder::Read;
using W = Recorder::Write;

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

using New = FlagReg<WriteBackActs<0x0000000FU>>;
using Old = FlagReg<NormalMode>;

// reserved bits 31:24 and the read-only bit read 1 here, two flags are pending, mode is 5
constexpr unsigned ReadBack = 0xAB10'5106U;

static_assert(Detail::GetAddress<New::Addr>::writeBackActsMask == 0x0000000FU);
static_assert(
  Detail::GetAddress<Old::Addr>::writeBackActsMask == 0xFFFF0EFFU,
  "a register without WriteBackActs zeroes the whole write-ignored-if-zero mask, as before");

void setKeepsReservedAndReadOnly() {
    test("set(): reserved and read-only bits as read, pending flags written 0");
    recorder.setReadValue(New::Addr::value, ReadBack);
    apply(set(New::en));
    checkActions({
      R{New::Addr::value,                       ReadBack},
      W{New::Addr::value, (ReadBack & ~0xFU) | (1U << 8)}
    });
}

void clearTouchesOnlyTheField() {
    test("clear(): the field's own bits, nothing else");
    recorder.setReadValue(New::Addr::value, ReadBack);
    apply(clear(New::en));
    checkActions({
      R{New::Addr::value,                      ReadBack},
      W{New::Addr::value, ReadBack & ~0xFU & ~(1U << 8)}
    });
}

void literalWrite() {
    test("write(field, literal): the field's zeros cleared, its ones set");
    recorder.setReadValue(New::Addr::value, ReadBack);
    apply(write(New::mode, value<std::uint32_t, 0xAU>()));
    checkActions({
      R{New::Addr::value,                                ReadBack},
      W{New::Addr::value, (ReadBack & ~0xFU & ~0xF000U) | 0xA000U}
    });
}

void runtimeWrite() {
    test("write(field, runtime): the whole field cleared first");
    recorder.setReadValue(New::Addr::value, ReadBack);
    apply(write(New::mode, runtimeValue(0x3U)));
    checkActions({
      R{New::Addr::value,                                ReadBack},
      W{New::Addr::value, (ReadBack & ~0xFU & ~0xF000U) | 0x3000U}
    });
}

void flagClearWritesOnlyThatFlag() {
    test("write() of one flag: that flag 1, the other pending flag 0");
    recorder.setReadValue(New::Addr::value, ReadBack);
    apply(write(New::flags, value<std::uint32_t, 0x2U>()));
    checkActions({
      R{New::Addr::value,                  ReadBack},
      W{New::Addr::value, (ReadBack & ~0xFU) | 0x2U}
    });
}

void oldModeZeroesEverything() {
    test("without WriteBackActs: every write-ignored-if-zero bit written 0 (the old code)");
    recorder.setReadValue(Old::Addr::value, ReadBack);
    apply(set(Old::en));
    checkActions({
      R{Old::Addr::value,                              ReadBack},
      W{Old::Addr::value, (ReadBack & ~0xFFFF0EFFU) | (1U << 8)}
    });
}
}   // namespace

int main() {
    setKeepsReservedAndReadOnly();
    clearTouchesOnlyTheField();
    literalWrite();
    runtimeWrite();
    flagClearWritesOnlyThatFlag();
    oldModeZeroesEverything();

    if(Kvasir::Test::failures != 0) {
        std::print("{} checks failed\n", Kvasir::Test::failures);
        return 1;
    }
    return 0;
}
