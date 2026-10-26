#pragma once
#include "c.hpp"        // resolves next to this file, same layer
#include "high/b.hpp"   // upward: NEW

#include <logl/log.hpp>   // log from a log = false layer: NEW
#include <vector>         // not in the spelling map: ignored
/* #include "high/c.hpp" in a comment */
// #if __has_include("high/c.hpp") in a comment
#if __has_include("high/b.hpp")   // a switch in a has_include = false layer: NEW
#endif

namespace low::detail {
struct Own {};

inline int helper(int x) { return x + 1; }
}   // namespace low::detail

namespace low {
namespace detail {
    template<typename T>
    struct Nested {};
}   // namespace detail

inline detail::Own own{};   // same repository: not a finding
}   // namespace low

#if defined(LOW_LOG)                                                                  \
  && __has_include("high/c.hpp")   // the build's switch + an existence check: exempt
#elif defined LOW_LOG                                                             \
  && __has_include(<logl/log.hpp>)   // the same over a line continuation: exempt
#endif
#if defined(OTHER) && __has_include("high/c.hpp")   // a macro the rule does not name: NEW
#endif
#if defined(LOW_LOG) || __has_include("logl/x.hpp")   // not the guard shape: NEW
#endif

namespace low { namespace detail {
    // a requires-clause between the template head and the name: still declarations of `recorded` and `twice`
    template<typename... Dummy>
        requires(sizeof...(Dummy) == 0)
    bool recorded() {
        return true;
    }

    template<typename T>
        requires std::is_integral_v<T> && (sizeof(T) > 1)
    T twice(T x) {
        return x + x;
    }
}}   // namespace low::detail
