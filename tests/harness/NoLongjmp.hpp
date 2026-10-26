// KVASIR_TEST_NO_LONGJMP: set under TSan, whose longjmp interceptor aborts on a jump out of a function that does not
// return ("can't find longjmp buf"). A case that leaves a panic by longjmp is left out there: `#ifndef
// KVASIR_TEST_NO_LONGJMP` around it (an #ifdef, not an if constexpr: the skipped body holds the setjmp). The one
// definition of the switch, for harness/PanicCatcher.hpp and the tests with stand-ins of their own.
#pragma once

#ifdef __SANITIZE_THREAD__
    #define KVASIR_TEST_NO_LONGJMP 1
#elifdef __has_feature
    #if __has_feature(thread_sanitizer)
        #define KVASIR_TEST_NO_LONGJMP 1
    #endif
#endif
