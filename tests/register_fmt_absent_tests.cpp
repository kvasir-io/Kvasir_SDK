// A logged register cannot print differently in a TU that lacks Register/RegisterFmt.hpp: there it has no formatter
// at all, a compile error. The log variants force RegisterFmt.hpp into every TU (util.cmake), so no chip header
// carries it; this TU deliberately does NOT include it, and stands for one that missed it.
//
// What used to happen there: a generated register is an empty aggregate and Register::Flags<R> one with a `value`
// member, so a reflection formatter (aglio's, `template<aglio::Described T>`, i.e. `glz::has_reflect<T>`) took them and
// printed other text. Both declare `glaze_reflect = false` now (svd_converter's template, Register/Flags.hpp). The
// stand-in below is aglio's constraint without aglio (not a dependency of the SDK tests).
#include "fixture_plain/CTRLP.hpp"
#include "kvasir/Register/Flags.hpp"
#include "kvasir_test.hpp"
#include "remote_fmt/remote_fmt.hpp"

#include <glaze/core/reflect.hpp>

namespace regfmt_absent {
// a reflectable struct, the control: the stand-in formatter takes it
struct Plain {
    int a;
};
}   // namespace regfmt_absent

// aglio's formatter in miniature: every struct glaze can reflect
template<typename T>
    requires glz::has_reflect<T>
struct remote_fmt::formatter<T> {};

namespace regfmt_absent {
template<typename T>
concept Formattable = requires { sizeof(remote_fmt::formatter<T>); };

using P = Kvasir::Peripheral::CTRLP::Registers<>;

static_assert(Formattable<Plain>,
              "the stand-in must take a reflectable struct, or the checks below prove nothing");
static_assert(!glz::has_reflect<P::INTFLAG>,
              "a generated register must not be reflectable");
static_assert(!glz::has_reflect<P::CH<1>>,
              "a generated register cluster element must not be reflectable");
static_assert(!glz::has_reflect<Kvasir::Register::Flags<P::INTFLAG>>,
              "Register::Flags must not be reflectable");
static_assert(!Formattable<P::INTFLAG>,
              "without RegisterFmt a logged register must not compile");
static_assert(!Formattable<P::GRP<1>::CFG>,
              "without RegisterFmt a logged register must not compile");
static_assert(!Formattable<Kvasir::Register::Flags<P::INTFLAG>>,
              "without RegisterFmt a logged Flags<> must not compile");
}   // namespace regfmt_absent

int main() {
    Kvasir::Test::test("a register without RegisterFmt has no formatter (static_asserts above)");
    return Kvasir::Test::report();
}
