#pragma once
#include "low/a.hpp"   // downward: fine

namespace high::detail {
struct Own {};
}   // namespace high::detail

namespace high {
inline int h = low::detail::helper(1);   // another repository's detail name: NEW
inline low::detail::Nested<int> n{};     // nested namespace, allow-listed: KNOWN
inline detail::Own              own{};   // both repositories declare Own; this one is its own
inline auto                     s = "low::detail::helper";   // in a string: ignored
// low::detail::helper in a comment: ignored
inline low::Detail::Own* p = nullptr;        // Detail is not detail: no declaration, not a finding
inline bool r = low::detail::recorded<>();   // declared with a requires(...) clause: NEW
inline int  t = low::detail::twice(2);       // declared with `requires C && (...)`: NEW
}   // namespace high
