#pragma once
// The register messages of KVASIR_STATIC_ASSERT (kvasir/Util/Diagnostic.hpp): they name the register and its fields
// when the address carries its register type (Register::Mode's second argument: always with a key, a read side effect
// or a write-only register, else only with NAMED_REGISTERS), else the address and the bits.
#include "Fields.hpp"
#include "kvasir/Util/Diagnostic.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace Kvasir::Register {
// Register/Utility.hpp; complete where a message is built
template<typename T>
struct RegisterTraits;
}   // namespace Kvasir::Register

namespace Kvasir::Register::Diagnostic {
using ::Kvasir::Diagnostic::Text;

/// The register type an address carries (void without one). `A` is a RegisterTraits<...>.
template<typename A>
struct RegisterOf {
    using type = void;
};

template<typename A>
    requires requires { typename A::Mode::Register; }
struct RegisterOf<A> {
    using type = typename A::Mode::Register;
};

/// "powman::vreg", "dma::ch[{}]::ctrl_trig": the generated fmt_string up to its field list.
template<typename R>
consteval std::string_view registerName() {
    if constexpr(requires { R::fmt_string; }) {
        std::string_view const f = R::fmt_string;
        return f.substr(0, f.find('('));
    } else {
        return {};
    }
}

/// "vsel, hiz" - the fields of R that `mask` touches; "bits 8:4" without a register type or field list.
template<typename R>
constexpr void fieldsIn(Text&    t,
                        unsigned mask) {
    bool any = false;
    // a generated register: names and its field list (Register/Fields.hpp)
    if constexpr(requires {
                     R::field_names;
                     typename R::fields;
                 })
    {
        for(std::size_t i = 0; i < R::field_names.size(); ++i) {
            if((fieldMasks<R>[i] & mask) != 0) {
                t << (any ? ", " : "") << R::field_names[i];
                any = true;
            }
        }
    }
    if(!any) {
        t << (std::has_single_bit(mask) ? "bit " : "bits ");
        t.bits(mask);
    }
}

/// "powman::vreg (0x4010000C)" or "register 0x4010000C"
template<typename A>
constexpr void where(Text& t) {
    using R = typename RegisterOf<A>::type;
    if constexpr(!std::is_void_v<R>) {
        t << registerName<R>() << " (";
        t.hex(static_cast<std::uint32_t>(A::address)) << ")";
    } else {
        t << "register ";
        t.hex(static_cast<std::uint32_t>(A::address));
    }
}

/// "field(s) vsel of powman::vreg (0x4010000C)", or "bits 11:4 of register 0x400A8004" without a register type
template<typename A>
constexpr void field(Text&    t,
                     unsigned mask) {
    using R = typename RegisterOf<A>::type;
    if constexpr(!std::is_void_v<R>) { t << "field(s) "; }
    fieldsIn<R>(t, mask);
    t << " of ";
    where<A>(t);
}

// used by Exec.hpp

template<typename A, unsigned ClearMask>
struct RmwRefused {
    consteval Text operator()() const {
        constexpr auto s = A::Mode::semantics;
        using R          = typename RegisterOf<A>::type;
        Text t;
        t << "write of ";
        where<A>(t);
        t << " field(s) ";
        fieldsIn<R>(t, ClearMask);
        t << " is a read-modify-write, which the register does not allow: ";
        if constexpr(s.readSideEffect) {
            t << "reading it has a side effect - write the whole register (FULLREGISTER) "
                 "(Register::Semantics::readSideEffect)";
        } else if constexpr((s.writeOnlyNoIdentity & ~ClearMask) != 0) {
            t << "it would write back the write-only field(s) ";
            fieldsIn<R>(t, s.writeOnlyNoIdentity & ~ClearMask);
            t << " (";
            t.hex(s.writeOnlyNoIdentity & ~ClearMask);
            t << "), which cannot be read: name them in the same write, or classify them in the "
                 "SVD (oneToSet / key / "
                 "accepted)";
        } else {
            t << "key field(s) ";
            fieldsIn<R>(t, s.mustSupply & ~ClearMask);
            t << " (";
            t.hex(s.mustSupply & ~ClearMask);
            t << ") read back something else than must be written - name the key in the same "
                 "write, or write "
                 "FULLREGISTER (Register::Semantics::mustSupply)";
        }
        return t;
    }
};

/// a partial write of a write-only register (Semantics::writeOnlyRegister) with a key it does not name
template<typename A, unsigned ClearMask>
struct KeyNotNamed {
    consteval Text operator()() const {
        constexpr auto s = A::Mode::semantics;
        using R          = typename RegisterOf<A>::type;
        Text t;
        t << "write of ";
        where<A>(t);
        t << " field(s) ";
        fieldsIn<R>(t, ClearMask);
        t << " does not name the key field(s) ";
        fieldsIn<R>(t, s.mustSupply & ~ClearMask);
        t << " (";
        t.hex(s.mustSupply & ~ClearMask);
        t << "): the register is write-only, so it is never read and the key would be written as 0 "
             "- name the "
             "key in the same write (Register::Semantics::mustSupply)";
        return t;
    }
};

template<typename A, unsigned Mask, unsigned Data>
struct BadMask {
    consteval Text operator()() const {
        Text t;
        t << "value ";
        t.hex(Data);
        t << " has bits outside the mask ";
        t.hex(Mask);
        t << " of ";
        field<A>(t, Mask);
        return t;
    }
};

// used by Factories.hpp

template<typename A, unsigned Mask, unsigned Value>
struct LiteralTooWide {
    consteval Text operator()() const {
        Text t;
        t << "literal ";
        t.dec(Value);
        t << " does not fit ";
        field<A>(t, Mask);
        t << " (it holds 0..";
        t.dec(Mask >> std::countr_zero(Mask));
        t << ") - bits above the field's width would be shifted out and silently lost";
        return t;
    }
};

template<typename A, unsigned Mask>
struct NotWritable {
    consteval Text operator()() const {
        Text t;
        t << "Access violation: ";
        field<A>(t, Mask);
        t << " is not marked as writable";
        return t;
    }
};

/// read(), readWhole, Array::readWhole
template<typename A, unsigned Mask>
struct NotReadable {
    consteval Text operator()() const {
        Text t;
        t << "Access violation: ";
        field<A>(t, Mask);
        t << " is not marked as readable";
        return t;
    }
};

/// The factory a single-bit check belongs to.
enum class Verb { set, clear, toggle };

template<typename A, unsigned Mask, Verb What>
struct NotSingleBit {
    consteval Text operator()() const {
        Text t;
        t << "Register::"
          << (What == Verb::set     ? "set"
              : What == Verb::clear ? "clear"
                                    : "toggle")
          << " only works on single bits, ";
        field<A>(t, Mask);
        t << " is ";
        t << (std::has_single_bit(Mask) ? "bit " : "bits ");
        t.bits(Mask);
        t << ": use Register::write to write values to wider bit fields";
        return t;
    }
};

template<typename A, unsigned Mask>
struct ClearOnFlag {
    consteval Text operator()() const {
        Text t;
        t << "Register::clear writes a 0, which ";
        field<A>(t, Mask);
        t << " ignores (write-one-to-clear or write-one-to-set): use Register::reset to clear a "
             "flag";
        return t;
    }
};

template<typename A, unsigned Mask>
struct ToggleNeedsRead {
    consteval Text operator()() const {
        Text t;
        t << "Register::toggle reads the bit and writes it back inverted: ";
        field<A>(t, Mask);
        t << " must be readable and writable";
        return t;
    }
};

/// clear() of a one-to-toggle field writes its current value back (a XOR of 0): it needs a read
template<typename A, unsigned Mask>
struct WriteOnlyToggleClear {
    consteval Text operator()() const {
        Text t;
        t << "Register::clear on a write-only toggle field: ";
        field<A>(t, Mask);
        t << " cannot be read back, and a written 0 toggles nothing";
        return t;
    }
};

template<typename A, unsigned Mask>
struct NotSetToClear {
    consteval Text operator()() const {
        Text t;
        t << "Register::reset writes ones to clear a flag: ";
        field<A>(t, Mask);
        t << " is not write-one-to-clear (oneToClear in the SVD); use clear() or write()";
        return t;
    }
};

/// write(field, value<T, V>()) with a T other than the field's type
template<typename A, unsigned Mask>
struct ValueTypeMismatch {
    consteval Text operator()() const {
        Text t;
        t << "type mismatch: the compile time value written to ";
        field<A>(t, Mask);
        t << " must have the field's type (value<FieldType, V>())";
        return t;
    }
};

/// readWhole / writeWhole given a field instead of the register's FULLREGISTER
template<typename A, unsigned Mask>
struct NotFullRegister {
    consteval Text operator()() const {
        Text t;
        t << "readWhole and writeWhole take the register's FULLREGISTER, not ";
        field<A>(t, Mask);
        return t;
    }
};

// used by AtomicFactories.hpp / SharedIsr.hpp

/// fieldAt<Bit>(R{}) found Count fields of R holding the bit (one is right)
template<typename R, unsigned Bit, std::size_t Count>
struct FieldAtBit {
    consteval Text operator()() const {
        Text t;
        t << "fieldAt<";
        t.dec(Bit) << ">: bit ";
        t.dec(Bit) << " of ";
        if constexpr(!registerName<R>().empty()) {
            t << registerName<R>() << " (";
            t.hex(static_cast<std::uint32_t>(R::Addr::value)) << ")";
        } else {
            t << "register ";
            t.hex(static_cast<std::uint32_t>(R::Addr::value));
        }
        if constexpr(Bit >= 32U) {
            t << " is outside the register";
        } else if constexpr(Count == 0) {
            t << " is in no field (reserved, or not in the SVD)";
        } else {
            t << " is in ";
            t.dec(Count) << " fields: name the field";
        }
        return t;
    }
};

/// Trigger: the field is a one-to-set trigger (atomic takes its set())
template<typename A, unsigned Mask, bool Trigger>
struct AtomicOnFlag {
    consteval Text operator()() const {
        Text t;
        t << "Register::atomic on ";
        field<A>(t, Mask);
        if constexpr(Trigger) {
            t << ", a one-to-set trigger: it takes set() only (a written 0 does nothing)";
        } else {
            t << ", a one-to-clear/one-to-toggle or read-only field: use reset() for a flag";
        }
        return t;
    }
};

/// What: 'F' ClearFirst, 'L' ClearLast
template<typename A, unsigned Mask, char What>
struct NotWriteOneToClear {
    consteval Text operator()() const {
        Text t;
        t << (What == 'F' ? "ClearFirst" : "ClearLast") << " names ";
        field<A>(t, Mask);
        t << ", which is not write-one-to-clear in the generated header (its bits are not in the "
             "register's "
             "write-ignored-if-zero mask): writing 1 to it would not clear it, and the write would "
             "not be a single "
             "store";
        return t;
    }
};

/// apply(bundle, ...) or apply(list(bundle)): R::overrideDefaults(...) with a runtime value is a value apply() runs
/// alone (Register.hpp, Apply.hpp Defaulted)
struct DefaultedNotAlone {
    consteval Text operator()() const {
        Text t;
        t << "R::overrideDefaults(...) with a runtime value is a bundle that apply() takes as its "
             "only argument: "
             "not inside a list, not next to other arguments - give the other actions to "
             "overrideDefaults or to "
             "an apply() of their own";
        return t;
    }
};

/// The kind of a non-write action given to R::overrideDefaults (Register.hpp)
enum class NotAWrite { read, toggle, atomic, other };

/// R::overrideDefaults(read(R::b), ...): its default writes are those of the fields no action names, so a read, a
/// toggle or an atomic write of a field would silently drop that field's default write
template<typename A, unsigned Mask, NotAWrite What>
struct OverrideDefaultsNotAWrite {
    consteval Text operator()() const {
        Text t;
        t << "R::overrideDefaults(...) takes writes only (write, set, clear, reset): "
          << (What == NotAWrite::read     ? "a read"
              : What == NotAWrite::toggle ? "a toggle"
              : What == NotAWrite::atomic ? "an atomic write"
                                          : "a chip's own action")
          << " of ";
        field<A>(t, Mask);
        t << " would leave that field's default write out - give it to an apply() of its own";
        return t;
    }
};

/// result[field] / get(field, result) of an apply() that did not read that field. FL is FieldLocation, which this
/// header (included by Types.hpp) cannot name.
template<typename TField>
struct FieldNotRead;

template<template<typename, unsigned, typename, typename> class FL,
         typename A,
         unsigned Mask,
         typename Acc,
         typename T>
struct FieldNotRead<FL<A, Mask, Acc, T>> {
    consteval Text operator()() const {
        Text t;
        t << "result[field]: ";
        field<RegisterTraits<A>>(t, Mask);
        t << " was not read in this apply(); add read(field) to it, or index a field it reads";
        return t;
    }
};
}   // namespace Kvasir::Register::Diagnostic
