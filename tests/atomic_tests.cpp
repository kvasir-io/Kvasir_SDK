// Register::atomic: literal writes only, refused on one-to-* fields; the default executor is the
// read-modify-write (interrupts masked on a target); the RP backend (chip_rp_common/RegisterAlias.hpp)
// turns a set into one store to +0x2000, a clear into one store to +0x3000, both into read + xor.
#include "test_registers.hpp"

#include <rp_common_alias/RegisterAlias.hpp>

using namespace Kvasir::Register;
using namespace Kvasir::Test;
using R = Recorder::Read;
using W = Recorder::Write;

namespace {
// a register inside the RP2350 DMA block (0x50000000, an aliased block)
struct DmaLikeReg {
    using Addr = Address<0x50000404, 0x00000000, 0x00000000, std::uint32_t>;

    static constexpr FieldLocation<Addr, maskFromRange(3, 3), ReadWriteAccess, std::uint32_t> ch3{};
    static constexpr FieldLocation<Addr, maskFromRange(5, 5), ReadWriteAccess, std::uint32_t> ch5{};
    static constexpr FieldLocation<Addr, maskFromRange(11, 8), ReadWriteAccess, std::uint32_t>
      level{};
};

static_assert(Kvasir::Rp::hasAtomicAlias(0x50000404));
static_assert(!Kvasir::Rp::hasAtomicAlias(0xd0000014),
              "SIO has no aliases");
static_assert(!Kvasir::Rp::hasAtomicAlias(0x50002404),
              "the alias window itself is not a register");

// atomic(set): one store to the SET alias, no read
void atomicSetIsOneStore() {
    test("atomicSetIsOneStore");
    apply(atomic(set(DmaLikeReg::ch3)));
    checkActions({
      W{0x50000404 + 0x2000, 1U << 3}
    });
}

// atomic(clear): one store to the CLR alias
void atomicClearIsOneStore() {
    test("atomicClearIsOneStore");
    apply(atomic(clear(DmaLikeReg::ch5)));
    checkActions({
      W{0x50000404 + 0x3000, 1U << 5}
    });
}

// set and clear of one register merge: ones and zeros both, so read + xor of what differs
void atomicSetAndClearMerge() {
    test("atomicSetAndClearMerge");
    recorder.setReadValue(0x50000404, 1U << 5);
    apply(atomic(set(DmaLikeReg::ch3), clear(DmaLikeReg::ch5)));
    checkActions({
      R{         0x50000404,               1U << 5},
      W{0x50000404 + 0x1000, (1U << 3) | (1U << 5)}
    });
}

// a multi-bit value: read, xor exactly the bits that differ
void atomicFieldValue() {
    test("atomicFieldValue");
    using levelT = typename decltype(DmaLikeReg::level)::DataType;
    recorder.setReadValue(0x50000404, 0x0000'0A28);
    apply(atomic(write(DmaLikeReg::level, value<levelT, 0x3>())));
    checkActions({
      R{         0x50000404, 0x0A28},
      W{0x50000404 + 0x1000, 0x0900}
    });
}

// one-to-* fields are refused, as a trait (a failing static_assert cannot be tested here)
static_assert(Detail::identityOfAccess<std::remove_cvref_t<decltype(CtrlReg::flag)>::Access>
              == Detail::Identity::zero);
}   // namespace

int main() {
    atomicSetIsOneStore();
    atomicClearIsOneStore();
    atomicSetAndClearMerge();
    atomicFieldValue();
    return report();
}
