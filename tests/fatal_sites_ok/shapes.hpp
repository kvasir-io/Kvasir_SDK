// Fixtures for tools/check_fatal_sites.py that must pass: right shapes it must not flag. Never compiled.
#define FIXTURE_LOGS(x)                           \
    do { UC_LOG_E("in a macro {}", x); } while(0)

char const        open  = '{';
char const* const close = "}";

namespace Policy {
template<unsigned Limit>
struct PanicAfter : Base<(Limit > 1), sizeof...(Limit)> {
    static void mismatch(unsigned got,
                         unsigned want,
                         unsigned inARow) {
        bool const fatal = inARow >= Limit;
        if(fatal) { ::Kvasir::Panic::beginFatal(); }
        UC_LOG_C("check: 0x{:08x} vs 0x{:08x} ({} of {})", got, want, inARow, Limit);
        if(fatal) { Kvasir::Panic::raiseAt(Kvasir::Panic::Cause::abort, 0, got); }
    }
};
}   // namespace Policy

static void logsOnly() { UC_LOG_I("a report"); }

extern "C" {
void otherLogs() { UC_LOG_W("{"); }

[[noreturn]] void raisesWithoutLog() { Kvasir::Panic::raise(Kvasir::Panic::Cause::abort); }
}

[[noreturn]] void beginsInNoreturn() {
    ::Kvasir::Panic::beginFatal();
    UC_LOG_C("then a loop");
    while(true) {}
}

void beginsWithMarker() {
    ::Kvasir::Panic::beginFatal();   // fatal end: the watchdog
    UC_LOG_C("watchdog follows");
}

static void assertsWithoutLog(bool up) { assert(up); }

// the fatal-site primitive: the macro is the start, the line and the end
[[noreturn]] void macroSite(int status) {
    UC_LOG_SCOPE_MODULE("assert");
    KVASIR_DETAIL_FATAL_AT(Kvasir::Panic::Cause::abort,
                           KVASIR_DETAIL_CALLER_PC(),
                           static_cast<std::uint32_t>(status),
                           "exit({}) called",
                           status);
}

[[noreturn]] void macroWithWrapperLog() {
    KVASIR_DETAIL_FATAL(UBSAN_REPORT(),
                        ::Kvasir::Panic::raiseAt(::Kvasir::Panic::Cause::undefinedBehaviour,
                                                 KVASIR_DETAIL_CALLER_PC()));
}

// log_assert starts the fatal path and logs; the raise follows
[[noreturn]] void assertFunc(char const* file,
                             int         line,
                             char const* expr) {
    uc_log::log_assert(line, file, expr);
    Kvasir::Panic::raiseAt(Kvasir::Panic::Cause::assertion, KVASIR_DETAIL_CALLER_PC());
}

// the macro inside an if, in a helper that raises for its caller
inline void* allocateOrPanic(void* block,
                             void* caller) {
    if(block == nullptr) {
        KVASIR_DETAIL_FATAL_AT(Kvasir::Panic::Cause::allocation,
                               ::Kvasir::Panic::Detail::sitePc(caller),
                               0,
                               "operator new: the heap is exhausted");
    }
    return block;
}

void* operatorNew(std::size_t size) {
    return allocateOrPanic(std::malloc(size), __builtin_return_address(0));
}

// a #define body through the primitive, and a wrapper's own definition (a definition is not a use)
#define FIXTURE_FATAL_CHECK(cond)                                                 \
    do {                                                                          \
        if(!(cond)) {                                                             \
            KVASIR_DETAIL_FATAL(KVASIR_DETAIL_CHECK_LOG(crit, "check failed"_sc), \
                                ::Kvasir::Check::Detail::failed());               \
        }                                                                         \
    } while(false)
#define UBSAN_REPORT()                                 \
    do { UC_LOG_E("UB at {}", nullptr); } while(false)
#define FIXTURE_THROW(function)                                                           \
    void function() { libstdcxxThrow("bad_cast", nullptr, __builtin_return_address(0)); }
