// The sanitizer counter Kvasir::Ubsan::ubsanReports against uc_log's printer (printer/TargetRecords.hpp `Ubsan`,
// `ubsanSize`), which reads it for `kvasir_bench.py ub`. Compiled only, never linked or run: ubsan.hpp defines the
// __ubsan_handle_* handlers, which a host program built with a sanitizer runtime already has. A drift fails the
// compile of this file, which kvasir_test_records_cross depends on (tests/CMakeLists.txt). Without USE_UC_LOG: uc_log's
// no-log front end (string_constant only) is enough for the struct, and the record is the same in every variant.
#include "kvasir/Runtime/ubsan.hpp"
#include "uc_log/printer/TargetRecords.hpp"

#include <cstddef>

namespace {
namespace records = uc_log::printer::records;
using Reports     = Kvasir::Ubsan::UbsanReports;

static_assert(sizeof(Reports) == records::ubsanSize,
              "the printer reads ubsanReports by records::UbsanSize");
constexpr std::size_t countAt  = offsetof(Reports, count);
constexpr std::size_t returnAt = offsetof(Reports, lastReturnAddress);
static_assert(countAt == 0,
              "records::decodeUbsan reads the count at 0");
static_assert(returnAt == 4,
              "records::decodeUbsan reads the last return address at 4");
static_assert(sizeof(records::Ubsan{}.count) == sizeof(Reports{}.count));
static_assert(sizeof(records::Ubsan{}.lastReturnAddress) == sizeof(Reports{}.lastReturnAddress));
static_assert(records::ubsanSymbol == "Kvasir::Ubsan::ubsanReports");
}   // namespace
