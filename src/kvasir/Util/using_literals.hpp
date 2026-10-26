#pragma once
// The firmware convenience header: Kvasir's literals (`_u32`, `_b`, ...) and the standard ones (`100ms`, `"x"sv`,
// ...) at global scope, for a firmware's .cpp and its own headers. Never include it from a library header: a
// library header says what it uses, inside its own namespace or function (`using namespace Kvasir::literals;`,
// `using namespace std::chrono_literals;`), and `Kvasir::literals` itself carries only Kvasir's literals.
#include "literals.hpp"

#include <chrono>
#include <string>
#include <string_view>

#ifdef __clang__
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wheader-hygiene"
#endif

using namespace Kvasir::literals;
using namespace std::literals;

#ifdef __clang__
    #pragma clang diagnostic pop
#endif
