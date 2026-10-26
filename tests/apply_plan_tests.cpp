// apply()'s constexpr plan (Register/Apply.hpp, Detail::Plan), checked at compile time:
// - the executed action types: a lone action keeps its own type, a merged one is Action<MergedLocation<...>, kind>;
// - the budget: a 512-action list plans with the default constexpr limits.
#include "test_registers.hpp"

#include <cstddef>
#include <type_traits>
#include <utility>

using namespace Kvasir::Register;
namespace D  = Kvasir::Register::Detail;
namespace br = brigand;

// ---- executed action types ----------------------------------------------------------------------------------

template<typename P, typename Seq>
struct ExecutedImpl;

template<typename P, std::size_t... I>
struct ExecutedImpl<P, std::index_sequence<I...>> {
    using type = br::list<typename D::AccessOf<P, I>::action...>;
};

template<typename... Args>
using Executed =
  typename ExecutedImpl<D::PlanOf<Args...>,
                        std::make_index_sequence<D::PlanOf<Args...>::plan.count>>::type;

using EnSet  = decltype(set(CtrlReg::en));
using IrqSet = decltype(set(CtrlReg::irq));
using DivRt  = decltype(write(CtrlReg::div, 1U));
using Data7  = decltype(write(SecondReg::data, value<std::uint32_t, 7>()));

// a lone action keeps its type
static_assert(std::is_same_v<Executed<EnSet>,
                             br::list<EnSet>>);
// two literals of one register: one merged access
static_assert(std::is_same_v<Executed<EnSet,
                                      IrqSet>,
                             br::list<Action<D::MergedLocation<CtrlReg::Addr,
                                                               0x3>,
                                             WriteLiteralAction<0x3>>>>);
// a literal and a runtime write: literal+runtime, carrying the runtime bits
static_assert(std::is_same_v<Executed<EnSet,
                                      DivRt>,
                             br::list<Action<D::MergedLocation<CtrlReg::Addr,
                                                               0xFF1>,
                                             WriteRuntimeAndLiteralAction<0x1,
                                                                          0xFF0>>>>);
// address descending; a sequencePoint splits
static_assert(std::is_same_v<Executed<EnSet,
                                      Data7>,
                             br::list<EnSet,
                                      Data7>>);
static_assert(std::is_same_v<Executed<Data7,
                                      EnSet>,
                             br::list<EnSet,
                                      Data7>>);
static_assert(std::is_same_v<Executed<Data7,
                                      SequencePoint,
                                      EnSet>,
                             br::list<Data7,
                                      EnSet>>);
// an empty step is no step
static_assert(D::PlanOf<SequencePoint,
                        SequencePoint,
                        EnSet,
                        SequencePoint>::plan.acc[0]
                .step
              == 0);
// the runtime inputs of a merged access: ascending argument indices
static_assert(std::is_same_v<D::AccessOf<D::PlanOf<decltype(write(ThirdReg::config,
                                                                  1U)),
                                                   EnSet,
                                                   decltype(write(ThirdReg::control,
                                                                  1U))>,
                                         1>::inputs,
                             D::Inputs<0,
                                       2>>);

// ---- budget ---------------------------------------------------------------------------------------------------

template<std::size_t... I>
constexpr auto alternating(std::index_sequence<I...>) {
    return Kvasir::MPL::list(
      br::list<decltype(write(CtrlReg::div, value<std::uint32_t, I % 256>())),
               decltype(atomic(set(CtrlReg::irq)))>{}...);
}

using Long = decltype(alternating(std::make_index_sequence<256>{}));   // 512 actions, none merges
static_assert(D::PlanOf<Long>::plan.count == 512);

template<std::size_t... I>
constexpr auto sameSet(std::index_sequence<I...>) {
    return Kvasir::MPL::list((static_cast<void>(I), set(CtrlReg::en))...);
}

static_assert(D::PlanOf<decltype(sameSet(std::make_index_sequence<512>{}))>::plan.count == 1);

int main() { return 0; }
