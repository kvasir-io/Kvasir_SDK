// The fixture's registers generated with NAMED_REGISTERS (svd_converter --named-registers=on): every register's
// Address names its register struct (Register::Mode<..., R>), so every register diagnostic names its fields. The
// instantiate_all.py TU next to this one instantiates every register of this variant.
#include "fixture_named/CTRLP.hpp"
#include "kvasir_test.hpp"

#include <type_traits>

using namespace Kvasir::Register;
using P = Kvasir::Peripheral::CTRLP::Registers<>;

static_assert(std::is_same_v<RegisterTraits<P::CTRL::Addr>::Mode::Register,
                             P::CTRL>,
              "a plain register names itself");
static_assert(std::is_same_v<RegisterTraits<P::GRP<0>::GRP_::Addr>::Mode::Register,
                             P::GRP<0>::GRP_>,
              "a group's register names itself");
static_assert(std::is_same_v<RegisterTraits<P::KEYREG::Addr>::Mode::Register,
                             P::KEYREG>,
              "a register with a key names itself in every mode");

int main() {
    Kvasir::Test::test("named registers: a write as in the plain variant");
    Kvasir::Test::recorder.setReadValue(0x40000000U, 0x0U);
    apply(set(P::CTRL::enable));
    Kvasir::Test::checkActions({
      Kvasir::Test::Recorder::Read{0x40000000U, 0x0U},
      Kvasir::Test::Recorder::Write{0x40000000U, 0x1U}
    });
    return Kvasir::Test::report();
}
