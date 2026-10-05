#pragma once

// KVASIR_SELFTEST: whether this translation unit evaluates a firmware's compile-time self-tests (static_asserts that
// simulate the code). They prove the same thing in every variant and can dominate compile time, so
// kvasir_executable_variants sets it to 1 only in SELFTEST_VARIANT (`debug` unless the firmware names another, or
// `all` / `none`) and 0 elsewhere. Builds without that function see the default below and check everything.
//
//     #include <kvasir/Util/SelfTest.hpp>
//     #if KVASIR_SELFTEST
//     namespace Test { ... static_assert(...); ... }
//     #endif
#ifndef KVASIR_SELFTEST
    #define KVASIR_SELFTEST 1
#endif
