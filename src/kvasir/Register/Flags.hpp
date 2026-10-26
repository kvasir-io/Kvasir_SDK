#pragma once

// "What is set", on one line: the single-bit fields present in `value`, named as the register names them --
// "arb_lost|abrt_txdata_noack", or "none" when nothing is set.
//
//   UC_LOG_W("i2c{} abort {}", instance, Kvasir::Register::Flags<Regs::IC_TX_ABRT_SOURCE>{cause});
//
// For status and cause registers: multi-bit fields (counters such as tx_flush_cnt) have no place in a flag list and
// are skipped. The names never travel -- each set flag costs the two bytes of its catalog id -- so this is what an
// ISR can afford, unlike the full-register dump the register's own formatter produces (one line per field, every
// field).
//
// A value type of the register layer, so it exists in every variant (a log call site names it even where the line
// compiles to nothing) and a driver includes only this; its formatter is in Register/RegisterFmt.hpp, which the log
// variants force into every TU (util.cmake; FlagPrintableRegister checks R there).
namespace Kvasir { namespace Register {
    template<typename R>
        requires requires {
            typename R::Addr::RegType;
            typename R::fields;
            R::field_names;
        }
    struct Flags {
        typename R::Addr::RegType value{};
        // no reflectable struct (as the registers, svd_converter): without RegisterFmt it has no formatter at all,
        // never a reflection formatter's "{value: ...}"
        static constexpr bool glaze_reflect = false;
    };
}}   // namespace Kvasir::Register
