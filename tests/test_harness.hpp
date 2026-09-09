// Minimal CHECK based test harness shared by all Kvasir tests.
//
// This header is deliberately free of any dependency on the register mock so it can be used
// by the pure host side tests (Util, Mpl, Atomic, ...) as well. Tests that do use the mock
// include kvasir_test.hpp instead, which installs the hooks below so that a failing check
// also dumps the recorded register accesses.
#pragma once

#include <cstddef>
#include <print>
#include <source_location>
#include <string_view>
#include <type_traits>
#include <utility>

namespace Kvasir { namespace Test {

    inline int              failures = 0;
    inline std::string_view currentTest{};

    // installed by kvasir_test.hpp to dump the recorded register accesses on a failure
    inline void (*failureContextPrinter)() = nullptr;
    // installed by kvasir_test.hpp to clear the recorder at the start of a test case
    inline void (*testResetHook)() = nullptr;

    inline void printFailureContext() {
        if(failureContextPrinter != nullptr) { failureContextPrinter(); }
    }

    inline void fail(std::string_view     msg,
                     std::source_location loc) {
        ++failures;
        std::print("FAIL [{}] {} ({}:{})\n", currentTest, msg, loc.file_name(), loc.line());
        printFailureContext();
    }

    // begin a new test case: names failure output and resets any registered state
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
        if(failures != 0) {
            std::print("{} checks failed\n", failures);
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

#define CHECK_EQ(a_, b_)                                                 \
    do {                                                                 \
        auto const checkEqA_ = ::Kvasir::Test::asUnsigned(a_);           \
        auto const checkEqB_ = ::Kvasir::Test::asUnsigned(b_);           \
        if(checkEqA_ != checkEqB_) {                                     \
            ++::Kvasir::Test::failures;                                  \
            std::print("FAIL [{}] {} == {}: 0x{:X} != 0x{:X} ({}:{})\n", \
                       ::Kvasir::Test::currentTest,                      \
                       #a_,                                              \
                       #b_,                                              \
                       checkEqA_,                                        \
                       checkEqB_,                                        \
                       std::source_location::current().file_name(),      \
                       std::source_location::current().line());          \
            ::Kvasir::Test::printFailureContext();                       \
        }                                                                \
    } while(false)
