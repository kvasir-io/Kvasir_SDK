// Must NOT compile (kvasir_generated_registers_rmw_must_fail): svd_converter's write-only guard on generated output.
// write_only/guard.svd's TC.CTRLA.SWRST is write-only and unclassified; converted with --write-only-mask=on its
// Mode carries the bit (Semantics::writeOnlyNoIdentity), and a write of ENABLE alone would read CTRLA and write SWRST
// back as read. The SDK must refuse it and name the field. Naming SWRST in the same write compiles (GENERATED_RMW_NAMES_SWRST).
#include "fixture_guard/TC.hpp"
#include "kvasir_test.hpp"

using namespace Kvasir::Register;
using CTRLA = Kvasir::Peripheral::TC::Registers<>::CTRLA;

int main() {
#ifdef GENERATED_RMW_NAMES_SWRST
    apply(set(CTRLA::enable), clear(CTRLA::swrst));
#else
    apply(set(CTRLA::enable));
#endif
    return 0;
}
