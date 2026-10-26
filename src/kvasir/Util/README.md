# kvasir/Util: base building blocks

Headers here include only the standard library, `kvasir/Mpl` and each other, so a host tool, a test or a library
without the runtime can use any of them. What an image needs at run time lives beside it:

| directory | holds |
|---|---|
| `kvasir/Runtime/` | fatal path (Panic, FaultHandler, Check, ubsan, StackProtector), crash and fault records, boot report and guard, image check, NOR stores, Executor, LoopProfiler, StackUsage, `Runtime::Standard` |
| `kvasir/Health/` | the Health supervisor (`Health.hpp` and its parts), LinkCheck, HealthKey, HealthRecords |
| `kvasir/Control/` | numeric filters (Average, Median, Interpolate, Pid, Ramp), std only |
| `kvasir/Log/` | RateLimiter, LogLimited, FaultLog: what driver log lines are built from |
| `kvasir/Port/` | the port contract and the named hooks (`Extends.hpp`) |

A new SDK header goes by its includes: std, Mpl or Util only -> here, anything else -> one of the directories above.
`tools/layers.toml` states the same per directory.
