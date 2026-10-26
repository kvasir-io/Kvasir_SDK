// Minimal CHECK based test harness shared by all Kvasir tests.
//
// This header is deliberately free of any dependency on the register mock so it can be used
// by the pure host side tests (Util, Mpl, Atomic, ...) as well. Tests that do use the mock
// include kvasir_test.hpp instead, which installs the hooks below so that a failing check
// also dumps the recorded register accesses.
//
// The one harness of the SDK's host tests, the threaded ones included: `failures` is atomic, so a CHECK in a worker
// thread is no race (TSan). Every FAIL line goes through failf ("FAIL [test] <text> (file:line)"), every program ends
// in `return Kvasir::Test::report();`. A test that catches panics adds harness/PanicCatcher.hpp; the time source of a
// test is support/FakeClock.hpp's FakeClockT<D> unless it must tick on reads (then a clock of its own).
#pragma once

#include <atomic>
#include <cstddef>
#include <format>
#include <print>
#include <source_location>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace Kvasir { namespace Test {

    // atomic: a CHECK in a worker thread of the multicore tests is no data race on the counter (TSan)
    inline std::atomic<int> failures{0};
    // owned: a name built at run time (kvasir_test.hpp's testFor) outlives the call that set it
    inline std::string currentTest{};

    // installed by kvasir_test.hpp to dump the recorded register accesses on a failure
    inline void (*failureContextPrinter)() = nullptr;
    // installed by kvasir_test.hpp to clear the recorder at the start of a test case
    inline void (*testResetHook)() = nullptr;

    inline void printFailureContext() {
        if(failureContextPrinter != nullptr) { failureContextPrinter(); }
    }

    // the one FAIL line: "FAIL [test] <text> (file:line)"; counts the failure, prints no context.
    // cold + noinline: the std::format FAIL path stays out of every CHECK site. Inlined, it bloated the test functions
    // until gcc -O3 stopped inlining the code under test (StaticVector::erase) and then warned -Wmaybe-uninitialized
    // for a pointer into its raw element storage passed to the out-of-line call.
    template<typename... A>
    [[gnu::cold,
      gnu::noinline]] void
    failf(std::source_location     loc,
          std::format_string<A...> f,
          A&&... a) {
        ++failures;
        std::print("FAIL [{}] {} ({}:{})\n",
                   currentTest,
                   std::format(f, std::forward<A>(a)...),
                   loc.file_name(),
                   loc.line());
    }

    [[gnu::cold,
      gnu::noinline]] inline void
    fail(std::string_view     msg,
         std::source_location loc = std::source_location::current()) {
        failf(loc, "{}", msg);
        printFailureContext();
    }

    // begin a new test case: names failure output and resets any registered state (copies the name)
    inline void test(std::string_view name) {
        currentTest = name;
        if(testResetHook != nullptr) { testResetHook(); }
    }

    template<typename T>
    constexpr unsigned long long asUnsigned(T v) {
        if constexpr(std::is_enum_v<T>) {
            return static_cast<unsigned long long>(std::to_underlying(v));
        } else {
            return static_cast<unsigned long long>(v);
        }
    }

    // returns the process exit code and prints a summary
    inline int report() {
        if(int const n = failures.load(); n != 0) {
            std::print("{} checks failed\n", n);
            return 1;
        }
        return 0;
    }

}}   // namespace Kvasir::Test

#define CHECK(...)                                                               \
    do {                                                                         \
        if(!(__VA_ARGS__)) {                                                     \
            ::Kvasir::Test::fail(#__VA_ARGS__, std::source_location::current()); \
        }                                                                        \
    } while(false)

#define CHECK_EQ(a_, b_)                                           \
    do {                                                           \
        auto const checkEqA_ = ::Kvasir::Test::asUnsigned(a_);     \
        auto const checkEqB_ = ::Kvasir::Test::asUnsigned(b_);     \
        if(checkEqA_ != checkEqB_) {                               \
            ::Kvasir::Test::failf(std::source_location::current(), \
                                  "{} == {}: 0x{:X} != 0x{:X}",    \
                                  #a_,                             \
                                  #b_,                             \
                                  checkEqA_,                       \
                                  checkEqB_);                      \
            ::Kvasir::Test::printFailureContext();                 \
        }                                                          \
    } while(false)
