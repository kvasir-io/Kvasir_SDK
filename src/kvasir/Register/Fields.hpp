#pragma once
// A generated register's facts as data, and the views on them that need nothing but the field types: svd_converter
// emits `using fields = Fields<decltype(f)...>` (every field location, a dim field once per element, in the order of
// fmt_string and field_names) and, per enumerated field, the names of its values. The views that execute
// (overrideDefaults, apply_fields) are in Register/Views.hpp, the log views in Register/RegisterFmt.hpp. This header
// is included by Register/Diagnostic.hpp, so it includes nothing of the register layer.
#include <array>
#include <cstddef>
#include <string_view>
#include <type_traits>

namespace Kvasir::Register {

/// The field locations of a register, as types (decltype of the generated static members).
template<typename... Fs>
struct Fields {
    static constexpr std::size_t size = sizeof...(Fs);
};

/// One named value of an enumerated field: svd_converter emits a `<FIELD>ValueNames` struct per such field whose
/// consteval `get()` returns them, and next to the enum a hidden friend `kvasirValueNames(<FIELD>Val)` returning
/// that struct, so the views find the names from the enum type alone (argument-dependent lookup). The elements of a
/// dim field share the struct.
struct ValueName {
    unsigned         value;
    std::string_view name;
};

namespace Detail {
    template<typename L>
    struct FieldMasks;

    template<typename... Fs>
    struct FieldMasks<Fields<Fs...>> {
        static constexpr std::array<unsigned, sizeof...(Fs)> value{
          std::remove_cvref_t<Fs>::mask...};
    };
}   // namespace Detail

/// Each field's mask, in the order of R::fields: compile-time data only (indexed at compile time it costs no flash).
template<typename R>
inline constexpr auto const& fieldMasks = Detail::FieldMasks<typename R::fields>::value;

}   // namespace Kvasir::Register
