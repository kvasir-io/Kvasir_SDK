#pragma once
#include "Register.hpp"

#if __has_include("remote_fmt/remote_fmt.hpp")
    #include "remote_fmt/remote_fmt.hpp"

    #if __has_include(<aglio/type_descriptor.hpp>)
        #include <aglio/type_descriptor.hpp>
    #endif

template<typename T>
  concept PrintableRegister = requires {
      { T::fmt_string } -> std::convertible_to<std::string_view>;
      {
          T::apply_fields([]<typename... Args>(Args&&...) { return true; })
      } -> std::convertible_to<bool>;
  }
    #if __has_include(<aglio/type_descriptor.hpp>)
  && aglio::Described<T>
    #endif
  ;

template<PrintableRegister R>
struct remote_fmt::formatter<R> {
    template<typename FormatContext>
    auto format(R const&,
                FormatContext& ctx) {
        return R::apply_fields_with_dim([&]<typename... Args>(Args&&... args) {
            return format_to(ctx.out(), SC_LIFT(R::fmt_string), std::forward<Args>(args)...);
        });
    }
};
#endif
