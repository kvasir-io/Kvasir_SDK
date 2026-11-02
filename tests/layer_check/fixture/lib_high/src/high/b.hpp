#pragma once
#include "low/a.hpp"   // downward: fine

namespace high::Detail {
struct Own {};
namespace Phase {
    inline int early = 1;
}
}   // namespace high::Detail

namespace high {
inline int h = low::Detail::helper(1);   // another repository's detail name: NEW
inline low::Detail::Nested<int> n{};     // nested namespace, allow-listed: KNOWN
inline Detail::Own              own{};   // both repositories declare Own; this one is its own
inline int e = Detail::Phase::early;   // a namespace this repository declares in Detail: not a finding
inline auto                     s = "low::Detail::helper";   // in a string: ignored
// low::Detail::helper in a comment: ignored
inline bool r = low::Detail::recorded<>();   // declared with a requires(...) clause: NEW
inline int  t = low::Detail::twice(2);       // declared with `requires C && (...)`: NEW
}   // namespace high
