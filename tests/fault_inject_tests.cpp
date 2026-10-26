// Kvasir::Fault::Inject (Runtime/FaultInject.hpp) on the mock: each helper is one 32-bit access, at the chip's
// Port::UnmappedAddress (the hook, specialised here as a chip package does) or at the given code address.
#include "kvasir/Common/Tags.hpp"
#include "kvasir/Port/Hooks.hpp"

#include <cstdint>

template<>
struct Kvasir::Port::UnmappedAddress<Kvasir::Tag::User> {
    static constexpr std::uintptr_t value = 0xF000'0000U;
};

#include "kvasir/Runtime/FaultInject.hpp"
#include "kvasir_test.hpp"

using namespace Kvasir::Test;

namespace {
void busFaults() {
    test(
      "busFaultLoad: one word read at Port::UnmappedAddress; busFaultStore: one word write there");
    Kvasir::Fault::Inject::busFaultLoad();
    Kvasir::Fault::Inject::busFaultStore();
    Kvasir::Fault::Inject::busFaultStore(0xDEAD'FA17U);
    checkActions({
      R{0xF0000000U,          0U},
      W{0xF0000000U,          1U},
      W{0xF0000000U, 0xDEADFA17U}
    });
}

void codeWrite() {
    test("writeToCode: one word write of 0xFFFFFFFF at the address, no read");
    Kvasir::Fault::Inject::writeToCode(runtimeValue(0x1000'0100U));
    checkActions({
      W{0x10000100U, 0xFFFFFFFFU}
    });
}
}   // namespace

int main() {
    busFaults();
    codeWrite();

    return Kvasir::Test::report();
}
