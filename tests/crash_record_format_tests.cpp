// A CrashRecord format of the application's own (the extension point, CrashRecord.hpp): its Panic part is a
// struct with recorded/cause/record/take, its Fault part and options are Full<>'s. Injected after the record headers, as an
// application does; every consumer (record, takeLastPanic, the parts' recorded()/cause(), takeLastFault) calls it.
#include "kvasir/Runtime/FaultRecord.hpp"
#include "kvasir/Runtime/Panic.hpp"

#include <cstdint>
#include <optional>
#include <type_traits>
#include <utility>

namespace {
struct CountingFormat {
    static constexpr Kvasir::CrashRecord::Options options{};

    struct Panic {
        static inline std::optional<Kvasir::Panic::FullRecord> kept{};
        static inline std::uint32_t ever = 0;   // the format's own extra: panics in all

        static bool recorded() { return kept.has_value(); }

        static std::optional<std::uint32_t> cause() {
            return kept ? std::optional<std::uint32_t>{kept->cause} : std::nullopt;
        }

        static void record(Kvasir::Panic::Info const& info) {
            ++ever;
            if(kept) {
                kept->count = kept->count + 1;
                return;
            }
            kept = Kvasir::Panic::FullRecord{.count    = 1,
                                             .cause    = static_cast<std::uint32_t>(info.cause),
                                             .pc       = info.pc,
                                             .detail   = info.detail,
                                             .core     = 0,
                                             .sp       = 0,
                                             .timeLow  = 0,
                                             .timeHigh = 0};
        }

        static std::optional<Kvasir::Panic::FullRecord> take() {
            return std::exchange(kept, std::nullopt);
        }
    };

    using Fault = Kvasir::CrashRecord::Full<>::Fault;
};

// a part without take(): not a PanicPart (an injected format like this is a compile error naming the concept)
struct NoTake {
    static bool recorded() { return false; }

    static std::optional<std::uint32_t> cause() { return std::nullopt; }

    static void record(Kvasir::Panic::Info const&) {}
};
}   // namespace

template<>
struct Kvasir::CrashRecord::RecordFormat<Kvasir::Tag::User> {
    using type = CountingFormat;
};

#include "test_harness.hpp"

static_assert(Kvasir::CrashRecord::Format<CountingFormat>);
static_assert(Kvasir::CrashRecord::PanicPart<CountingFormat::Panic>);
static_assert(!Kvasir::CrashRecord::PanicPart<NoTake>);
static_assert(std::is_same_v<Kvasir::CrashRecord::Records<struct AnyAnchor>,
                             CountingFormat>);

int main() {
    // the image's format, after its specialisation: what BootGuard and the boot report read
    using Records = Kvasir::CrashRecord::Records<struct RecordsAnchor>;
    using Kvasir::Panic::Cause;
    using Kvasir::Test::test;

    test(
      "the application's format records and gives back the panic; Full's record stays untouched");
    Kvasir::Panic::lastPanic.clear();
    Kvasir::Panic::record(Kvasir::Panic::Info{.cause = Cause::abort, .pc = 0x1000'0100U});
    Kvasir::Panic::record(Kvasir::Panic::Info{.cause = Cause::user, .pc = 0x1000'0200U});
    CHECK(Records::Panic::recorded());
    CHECK(Records::Panic::cause() == static_cast<std::uint32_t>(Cause::abort));
    CHECK(!Kvasir::Panic::lastPanic.valid());
    auto const p = Kvasir::Panic::takeLastPanic();
    CHECK(p && p->count == 2 && p->pc == 0x1000'0100U);
    CHECK(!Kvasir::Panic::takeLastPanic());
    CHECK(CountingFormat::Panic::ever == 2U);

    test("its Fault part is Full's: Fault::lastFault");
    Kvasir::Fault::FullRecord f{};
    f.count = 1;
    Kvasir::Fault::lastFault.store(f);
    CHECK(Kvasir::Fault::takeLastFault().has_value());
    CHECK(!Records::Fault::recorded());

    return Kvasir::Test::report();
}
