#pragma once
// The hook form's two pieces every layer names (Port/Hooks.hpp's table): the tag a chip or an application specialises
// a hook for, and Port::LateT, which names it so that the lookup waits for a dependent type. No includes.

namespace Kvasir { namespace Tag {
    struct User {};
}}   // namespace Kvasir::Tag

namespace Kvasir::Port {
// T, named so that the lookup waits for Dep: the member typedef of a class template is dependent (an alias template
// whose parameter is unused would not be). The SAME type as T, so mangled names do not change.
template<typename T, typename... Dep>
struct Late {
    using type = T;
};

template<typename T, typename... Dep>
using LateT = typename Late<T, Dep...>::type;
}   // namespace Kvasir::Port
