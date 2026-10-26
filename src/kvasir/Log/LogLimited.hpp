#pragma once

// KVASIR_LOG_LIMITED(decision, LOG, fmt, ...): a log line that only goes out when a limiter's decision allows it, with
// the occurrences dropped since the previous one appended (" (+N not logged)"), and the Suppressed formatter that
// renders that count. The limiters themselves are RateLimiter.hpp.
//
// Which definition is in force: in a log variant (USE_UC_LOG, set by the build) the real one; otherwise - a variant
// without the log, a host build - a no-op that names the decision and the arguments only in an unevaluated operand, so
// what the line mentions still counts as used. A host test that counts limited lines defines its own
// KVASIR_LOG_LIMITED before including anything (kvasir_devices test/kd/LogStubs.hpp): the no-op here is behind
// `#ifndef KVASIR_LOG_LIMITED`, so that one stays. This header brings uc_log/uc_log.hpp only in a log variant: a
// header that writes UC_LOG_* lines of its own includes it itself.

#include "kvasir/Log/RateLimiter.hpp"

#if defined(KVASIR_LOG_FORMATTERS) && __has_include("remote_fmt/remote_fmt.hpp")
    #include "remote_fmt/remote_fmt.hpp"

// Nothing for 0, " (+N not logged)" otherwise -- so a normal line stays as it was and
// only a line that stands for dropped ones carries the count.
template<>
struct remote_fmt::formatter<Kvasir::Suppressed> {
    template<typename Printer>
    constexpr auto format(Kvasir::Suppressed const& s,
                          Printer&                  printer) const {
        if(s.n == 0) { return format_to(printer, SC_LIFT("")); }
        return format_to(printer, SC_LIFT(" (+{} not logged)"), s.n);
    }
};
#endif

#if defined(USE_UC_LOG) && __has_include("uc_log/uc_log.hpp")
    #include "uc_log/uc_log.hpp"

    // Logs through LOG (UC_LOG_W, UC_LOG_E, ...) only when `decision` allows it, with
    // the occurrences dropped since the previous line appended to the same message:
    //
    //   KVASIR_LOG_LIMITED(faultLog_.allow(key), UC_LOG_W, "i2c{} abort {}", inst, src);
    //   -> "i2c0 abort ..."                    normally
    //   -> "i2c0 abort ... (+199 not logged)"  when 199 were suppressed before it
    //
    // `fmt` must be a string literal (it is concatenated with the count placeholder).
    #define KVASIR_LOG_LIMITED(decision, LOG, fmt, ...)                \
        do {                                                           \
            if(auto const kvasir_limited_ = (decision)) {              \
                LOG(fmt "{}" __VA_OPT__(, ) __VA_ARGS__,               \
                    ::Kvasir::Suppressed{kvasir_limited_.suppressed}); \
            }                                                          \
        } while(false)
#endif

#ifndef KVASIR_LOG_LIMITED
namespace Kvasir { namespace Detail {
    // Only named in an unevaluated operand (never defined): what a compiled-out limited line mentions counts as used.
    template<typename... Ts>
    int namedByLimitedLog(Ts const&...);
}}   // namespace Kvasir::Detail

    // Logging compiled out: the decision is never evaluated (a LogRateLimiter is empty then, and a real limiter's
    // bookkeeping would be work for nothing), nor are the arguments; LOG is not expanded.
    #define KVASIR_LOG_LIMITED(decision, LOG, fmt, ...)                                      \
        static_cast<void>(                                                                   \
          sizeof(::Kvasir::Detail::namedByLimitedLog((decision)__VA_OPT__(, ) __VA_ARGS__)))
#endif
