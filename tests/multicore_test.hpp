#pragma once
// The minimal harness the multicore tests share: a failure counter and CHECK, without the
// register recorder (kvasir_test.hpp), which is a non-atomic global and must not be touched
// from two threads.
#include <cstdio>
#include <source_location>
#include <string_view>

namespace Kvasir::Test {
inline int              failures = 0;
inline std::string_view currentTest{};

inline void test(std::string_view name) { currentTest = name; }

inline void fail(std::string_view     msg,
                 std::source_location loc = std::source_location::current()) {
    ++failures;
    std::printf("FAIL [%.*s] %.*s (%s:%u)\n",
                static_cast<int>(currentTest.size()),
                currentTest.data(),
                static_cast<int>(msg.size()),
                msg.data(),
                loc.file_name(),
                static_cast<unsigned>(loc.line()));
}

inline int finish() {
    if(failures != 0) {
        std::printf("%d checks failed\n", failures);
        return 1;
    }
    return 0;
}
}   // namespace Kvasir::Test

#define CHECK(...)                                                               \
    do {                                                                         \
        if(!(__VA_ARGS__)) {                                                     \
            ::Kvasir::Test::fail(#__VA_ARGS__, std::source_location::current()); \
        }                                                                        \
    } while(false)
