#pragma once
// The register messages of KVASIR_STATIC_ASSERT (kvasir/Util/Diagnostic.hpp): they name the register and its fields
// when the address carries its register type (always with an RmwHazard, else only with NAMED_REGISTERS), else the
// address and the bits.
#include "kvasir/Util/Diagnostic.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace Kvasir::Register::Diagnostic {
using ::Kvasir::Diagnostic::Text;

/// The register type an address carries (void without one). `A` is an Address<...> or a GetAddress<...>.
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
    if constexpr(requires {
                     R::field_names;
                     R::field_masks;
                 })
    {
        for(std::size_t i = 0; i < R::field_names.size(); ++i) {
            if((R::field_masks[i] & mask) != 0) {
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
        t.hex(static_cast<std::uint32_t>(A::value)) << ")";
    } else {
        t << "register ";
        t.hex(static_cast<std::uint32_t>(A::value));
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
        using Mode = typename A::Mode;
        using R    = typename RegisterOf<A>::type;
        Text t;
        t << "write of ";
        where<A>(t);
        t << " field(s) ";
        fieldsIn<R>(t, ClearMask);
        t << " is a read-modify-write, which the register does not allow: ";
        if constexpr(Mode::readHasSideEffect) {
            t << "reading it has a side effect - write the whole register (FULLREGISTER) "
                 "(Register::RmwHazard)";
        } else if constexpr((Mode::writeOnlyNoIdentityMask & ~ClearMask) != 0) {
            t << "it would write back the write-only field(s) ";
            fieldsIn<R>(t, Mode::writeOnlyNoIdentityMask & ~ClearMask);
            t << " (";
            t.hex(Mode::writeOnlyNoIdentityMask & ~ClearMask);
            t << "), which cannot be read: name them in the same write, or classify them in the "
                 "SVD (oneToSet / key / "
                 "accepted)";
        } else {
            t << "key field(s) ";
            fieldsIn<R>(t, Mode::mustSupplyMask & ~ClearMask);
            t << " (";
            t.hex(Mode::mustSupplyMask & ~ClearMask);
            t << ") read back something else than must be written - name the key in the same "
                 "write, or write "
                 "FULLREGISTER (Register::RmwHazard)";
        }
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

/// What: "set", "clear", "toggle"
template<typename A, unsigned Mask, char What>
struct NotSingleBit {
    consteval Text operator()() const {
        Text t;
        t << "Register::"
          << (What == 's'   ? "set"
              : What == 'c' ? "clear"
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

// used by AtomicFactories.hpp / SharedIsr.hpp

template<typename A, unsigned Mask>
struct AtomicOnFlag {
    consteval Text operator()() const {
        Text t;
        t << "Register::atomic on ";
        field<A>(t, Mask);
        t << ", a one-to-clear/one-to-set/one-to-toggle or read-only field: use reset() for a flag";
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
}   // namespace Kvasir::Register::Diagnostic
