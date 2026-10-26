# kvasir/Register: the register layer

Every access to a peripheral register goes through this layer. A generated register header (svd_converter, from the
chip's SVD) gives each register its address, its fields and its write facts as types; the layer turns field actions
into the fewest loads and stores that do exactly what was asked, checks at compile time what the hardware would
silently ignore, and lets a chip replace the access itself.

## The pieces, bottom up

| header | what |
|---|---|
| `Mmio.hpp` | the layer's one raw access: a load or store of a T at an address, always inlined; with `KVASIR_REGISTER_MOCK` it goes to the host mock (`Test::readAt` / `writeAt`) |
| `Types.hpp` | `Address`, `FieldLocation`, `Action`, the access kinds, and `Semantics` - a register's write facts (key bits, read side effects, write-only bits, `writeBackActs`, `fromReset`) |
| `Fields.hpp`, `Views.hpp` | a generated register's facts as data (its field list, enum names) and the views on them (`overrideDefaults`, the read of all fields) |
| `Utility.hpp` | `RegisterTraits`: a register's address, masks and whole-word `read()` / `write()` from any of its locations or actions; the field checks every factory asserts |
| `Factories.hpp`, `AtomicFactories.hpp` | the actions: `set`, `clear`, `toggle`, `write`, `read`, `reset` (a flag's clear), `Register::atomic(...)` |
| `Apply.hpp` | `apply(...)`: the planner - flattens the actions, orders and merges them into accesses, executes each |
| `Exec.hpp` | the executors of one access (a store, a read-modify-write, a masked read-modify-write for `atomic`) and the hook `ExecuteSeam` |
| `Register.hpp` | the umbrella: `value<>()`, `overrideDefaults`, `Register::fromReset<R>(...)`, `readWhole` / `writeWhole`, `extract` / `deposit`, `Register::Array` |
| `Memory.hpp` | memory that is no register (flash and XIP windows, OTP and NVM rows, ROM tables) as plain words of a width, through `Mmio` |
| `Wait.hpp`, `WaitLog.hpp` | bounded waits `waitUntil<Bound, Policy>(condition)`, the image's `WaitPolicy` hook, `OnTimeout::Log`'s main-loop line (runtime layer: they may panic and log) |
| `BitField.hpp` | a bit range of a byte image (an NVM row), not a register |
| `Diagnostic.hpp`, `Flags.hpp`, `RegisterFmt.hpp` | the compile-time messages that name a register and its fields; a one-line list of the set flags; the formatters (a log adapter, opt-in) |

## The field API is the rule

```cpp
apply(set(R::CTRL::enable), write(R::CTRL::mode, value<3>()));   // one read-modify-write, or one store
auto const ready = apply(read(R::STATUS::ready));                 // one load, the field's value
apply(reset(R::INTFLAG::done));                                    // a write-one-to-clear flag: one store of its bit
```

- Registers are written and read through `apply(set/clear/write/read/reset(...))` with the generated fields: no
  `reinterpret_cast`, no `readWhole(R) | F.mask`. The field form is checked (a read-only field written, a write-only
  field read, a literal wider than its field, `clear()` on a write-one-to-clear flag, a register whose read has a
  side effect read-modify-written: each a compile error that names the register) and it is the cheapest form: a
  form that costs more than a hand access is a defect of this layer, to be fixed here or in svd_converter.
- **In RAM functions too.** Everything on the execution path of `apply()` is always inlined, with no lambda and no
  `std::` call, so a function that runs with XIP off may call it (gcc at -Og and the sanitize variant included).
- `readWhole` / `writeWhole` (on the register's `FULLREGISTER`) only where the value really is the whole word: a
  data, FIFO or counter register, a snapshot of a status register written back to clear exactly the flags it saw,
  a runtime flag mask, a register array in a loop (`Register::Array`). A field of such a word is `extract` /
  `deposit`.
- The order of one `apply()`: steps separated by `sequencePoint` run in order; within a step accesses run by address
  (descending), reads before toggles before writes, and actions of one register merge into one access by the table
  at `mergeOf()`. A write that must follow another goes after a `sequencePoint` or into its own `apply()`.

## Write facts and the masks behind a field write

A field write is an OR or BIC of the field's own bits. What else the store carries comes from the register's
`Semantics` (svd_converter emits them from the SVD's modifiedWriteValues, writeConstraint and readAction):

- `mustSupply`: key bits every write names (they read back something else);
- `readSideEffect`: a read pops or clears something - only whole-register writes;
- `writeOnlyRegister` / `writeOnlyNoIdentity`: nothing (or some bits) cannot be read back - a partial write never reads
  the register, or must name those bits;
- `writeBackActs`: the write-one-to-* bits a read-modify-write must write as 0, so a pending flag is not cleared and a
  trigger not fired by writing back what was read. Reserved and read-only bits go back as read.

`Register::fromReset<R>(...)`: literal writes of R's fields as ONE store over R's reset value, never a read - for a
register only the boot writes, whose other fields must be their reset values anyway.

## `Register::atomic` and the chip's seam

`Register::atomic(set(A::x), clear(A::y))`: literal writes that must not lose a concurrent update of another field of
the same register (from an interrupt, or the other core). How is the chip's: every access of `apply()` goes through
the late hook `ExecuteSeam<Tag::User, Action>` (default: the plain executor; masked read-modify-write for an atomic
one), and a chip specialises it for an access kind on its addresses - a store to an RP chip's SET/CLR/XOR alias, an
STM32 ODR toggle through BSRR. The chip names the header in `KVASIR_CHIP_REGISTER_SEAM`; `Exec.hpp` includes it at
its end, so every TU that reaches `apply()` sees the chip's access before the first instantiation. An `ExecuteSeam`
specialisation keeps the action's meaning: the same bits end up written, only the instructions differ. Two writers
of the SAME field still race; atomic protects the other fields.

## Bounded waits

`waitUntil<Bound, Policy>(isSet(R::flag))`: the bound a datasheet time (`MicrosecondsBound<Us>` at the chip's
`CpuHzCeiling`, cited next to the call), a poll count or a deadline; the policy what a timeout does (`Unbounded`
compiles to the plain loop, `Report` returns the result to a caller that recovers, `Panic`, `Log`, `Count`, ...;
the image's choice is the hook `WaitPolicy<Tag::User>`). A wait before the memory init (a clock setup) names
`OnTimeout::Report` and writes no variable.

## Tests

The host tests build the layer against the mock (`KVASIR_REGISTER_MOCK`): `tests/apply_order_tests.cpp`,
`apply_plan_tests.cpp`, `merge_tests.cpp`, `factories_tests.cpp`, `register_wait_tests.cpp` and their must-fail cases.
