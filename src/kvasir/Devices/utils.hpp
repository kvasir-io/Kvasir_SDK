#pragma once

#include <variant>

namespace Kvasir::SM {
template<class... Ts>
struct overloaded : Ts... {
    using Ts::operator()...;
};
template<class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

template<typename Variant,
         typename... Matchers>
auto match(Variant&& variant,
           Matchers&&... matchers) {
    return std::visit(overloaded{std::forward<Matchers>(matchers)...},
                      std::forward<Variant>(variant));
}
}   // namespace Kvasir::SM
