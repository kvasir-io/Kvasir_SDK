// Kvasir::Panic::Cause against the copy that stays outside the SDK: uc_log's PanicCauses, the names
// the printer gives a lastPanic record it reads off the target (uc_log cannot include Panic.hpp - it
// is its own project, and the SDK includes it, not the other way round). Compiled only where the
// uc_log checkout is; a table that drifted fails the compile.
#include "kvasir/Util/Panic.hpp"

#include "uc_log/detail/TargetRecords.hpp"

#include <cstddef>
#include <string_view>

namespace {
namespace records = uc_log::detail::records;

constexpr bool namesMatch() {
    for(std::size_t i = 0; i != records::PanicCauses.size(); ++i) {
        if(records::PanicCauses[i]
           != std::string_view{Kvasir::Panic::name(static_cast<Kvasir::Panic::Cause>(i))})
        {
            return false;
        }
    }
    return true;
}

static_assert(records::PanicCauses.size() == Kvasir::Panic::CauseCount,
              "uc_log's PanicCauses misses a Cause, or has one too many");
static_assert(namesMatch(),
              "uc_log's PanicCauses is not Panic.hpp's name(), in Cause order");
static_assert(records::PanicMagic == Kvasir::Panic::Record::Magic);
}   // namespace

int main() { return 0; }
