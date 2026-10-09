# llvm_port: how Kvasir_SDK's libc, libc++ and compiler-rt are made

`libcport` turns an llvm-project checkout into `Kvasir_SDK/lib/{libc,libcxx,compiler-rt}` (each its own
submodule). Moved from `~/llvm_port` to kvasir_work on 2026-10-02 (local git repo, no remote), into
`Kvasir_SDK/tools/llvm_port` on 2026-10-08 (plain copy; the top-level `llvm_port/` is the old one). Every change Kvasir
makes to those submodules must be recorded here, or the next port drops it (rule in kvasir_work's `CLAUDE.md`).
Paths below are relative to this directory: the SDK is `../..`. An llvm-project checkout at the ported tag:
`~/.cache/kvasir/llvm-<tag>` (sparse, blobless clone of the tag; `~/repos/llvm-project` is gone since 2026-10-08).

## What lives here

| path | what |
|---|---|
| `libcport` | the tool (`--help`). Port mode mirrors upstream, applies patches, syncs the `.cmake` source lists, writes `<lib>/UPSTREAM`. `--save-patches` goes the other way. `--check` (2026-10-08) says whether a port at `UPSTREAM`'s tag would change anything. |
| `fetch-llvm` | an llvm-project at a tag (default: `lib/libc/UPSTREAM`'s): sparse blobless clone, cached in `~/.cache/kvasir/llvm-<tag>`, prints the path. |
| `tests/check_selftest.sh <llvm>` | `--check` against nine kinds of seeded drift on a scratch copy (17 s); Kvasir_SDK CI runs it monthly with the full check. |
| `kvasir/<lib>/patches/<path>.patch` | one patch per upstream file Kvasir changes (GNU patch, fuzz 2), each starting with its header block (`Kvasir patch:`, `Why:`, `Upstream:`, `Proof:`, `Drop when:`, format in `libcport --help`); `--save-patches` keeps the blocks and gives a new patch one full of TODOs, which `--check` refuses. |
| `kvasir/<lib>/owned.txt` | Kvasir-only files: never overwritten, never deleted by the mirror. |
| `kvasir/libc-config/exclude.txt` | regexes of libc targets taken OUT of upstream's `libc/config/baremetal/arm/{entrypoints,headers}.txt` (the lists are derived anew each port). |
| `libc-config/config.json` | our `LIBC_CONF_*` values; `OPTIONS.md` is regenerated from upstream each port. |
| `clear-new-marks` | strips the `# NEW` tags the source-list sync adds, after review. |

## Porting a new release (done for llvmorg-23.1.2 on 2026-10-02, llvmorg-23.1.3 on 2026-10-06)

0. Drift check first (2026-10-08): `just port-check` in kvasir_work (`./libcport --check $(./fetch-llvm) ../..`
   here). Exit 0 = the submodules are exactly upstream + patches + owned files, so the port loses nothing; each finding
   says what to do (EDIT -> `--save-patches`, UNOWNED -> owned.txt, ...). It reads the working tree, untracked owned
   dirs included - unlike the port's own refusal. Full: ~1 min (a headers-only libc build in a temp dir); `just
   port-check quick`: seconds, libc `include/` unchecked - not enough before a port.
   Needs cmake, make, clang and PyYAML (libc's hdrgen: `ModuleNotFoundError: No module named 'yaml'` otherwise).
   Then triage (2026-10-06, 23.1.2 -> 23.1.3): `git -C <llvm-project> diff --stat <old> <new> -- libc
   libcxx compiler-rt` shows what can reach us. libc math fixes inside `#ifndef LIBC_MATH_HAS_SKIP_ACCURATE_PASS`
   do not (config.json sets SKIP_ACCURATE_PASS); compiler-rt files absent from `lib/compiler-rt` do not either.
1. Submodules committed and clean (the port refuses otherwise). `--save-patches` first if anyone edited an
   upstream file since the last port: llvm-project at the tag in `<lib>/UPSTREAM`, and its `build/` from that port.
   Trap (2026-10-06): the check counts UNTRACKED files too (Kvasir-owned `kvasir/` dirs not yet committed), and it
   runs only AFTER the ~10 min libc build - check `git -C ../../lib/<lib> status --porcelain` is empty for
   all three first. Uncommitted Kvasir work that must not be committed yet: `git stash push -u` in each, port, `git
   stash pop` (dominic's choice that day).
2. An llvm-project at the new tag: `git -C <llvm-project> fetch origin tag llvmorg-X --no-tags && git checkout llvmorg-X`,
   or `./fetch-llvm llvmorg-X` (sparse, blobless; enough for the port: `runtimes/` configures libc from it).
3. `./libcport <llvm-project> ../..` (the whole port with its libc build: 33 s on 2026-10-08, from a `fetch-llvm` clone
   with `LIBC_BUILD_DIR` in a temp dir; earlier ports noted ~10 min). Read the summary at the end.
4. A patch that did not apply leaves a `.rej`: check whether upstream fixed it itself (then drop it), else merge by
   hand; delete the `.rej`s; `./libcport --save-patches ...` to rewrite the patch set from the result.
5. Review the `# NEW` entries in libc.cmake / libcxx.cmake / compiler-rt.cmake, enable what fits (libc: float/double
   math, ctype, string(s), stdlib basics; no f16/bf16/f128/long double), then `./clear-new-marks ../..`.
   "ACTIVE source(s) gone upstream" = look for what replaced them (22->23: math helpers became header-only).
6. Compare `libcxx/include/__config_site.in` and `vendor/llvm/default_assertion_handler.in` across the two tags:
   Kvasir's `__config_site` is empty, the values are `libcxx_profile_flags` in `cmake/arm_compiler_common.cmake`.
7. Check every selected source compiles, clang AND gcc, M0+ and M33, all variants, before any firmware build.
8. test_examples `113_libc_link_check` in every variant on an M33 and an M0+ tree, clang and gcc_llvm: it links every
   enabled libc entrypoint (`Kvasir_SDK/cmake/tools/libc_link_probe.py` from `libc.cmake`). An undefined symbol there
   is a source switched on without the object it needs; run it on a board too (`LIBC|DONE|...|fail=0`).

## What the 22.1.8 -> 23.1.2 port met

- `LIBC_CONF_TIME_64BIT` is gone (time_t always 64-bit, llvm b2a6da6): an unknown option in config.json fails
  cmake. The port now refuses it up front (OPTIONS.md generation).
- libc 23's stdio entrypoints depend on `stdin/stdout/stderr` entries: the old hand-copied entrypoints.txt
  (22 minus stdfix) failed configure. Hence the derived lists + exclude.txt.
- Generated headers' include guards became `_LLVM_LIBC_*`: patches anchored on the last line fail on the context.
- Upstream fixed four Kvasir patches itself: `vsqrt.f32/%P` (gcc), atan2f `MPI_OVER_2`, `stdfix-macros.h`
  (`&& defined(__clang__)`), `limits_macros.h` (full build uses libc's own `limits-macros.h`). `patch -N` reports
  those as "Reversed (or previously applied)".
- The libc build dir now holds `*.rsp` response files next to each header: pruned.
- libc++ 23 removed its C wrappers (`ctype.h fenv.h float.h inttypes.h`); `stdbool.h` was already gone, and gcc
  builds (`-nostdinc`) found it only in the stale copy - now a libc-owned `include/stdbool.h`.
- libc++ 23 wraps `std` in a `#pragma clang attribute push` ABI region: its own sources warn
  (`-Wpragma-clang-attribute`, `-Wignored-attributes`) - suppressed in libcxx.cmake; user code sees system headers.
- compiler-rt: `arm/addsf3.S` is now the Thumb-2 optimized add; the old Thumb-1 one moved to `arm/thumb1/addsf3.S`.
  Kvasir now takes upstream's optimized soft-float (`COMPILER_RT_ARM_OPTIMIZED_FP`): one set per instruction set,
  lists in compiler-rt.cmake; the superseded C files include `arm/aeabi_frsub.c`/`aeabi_drsub.c` (found by a
  duplicate-symbol check: compile the selected files per core, `llvm-nm --defined-only -g`, `uniq -d`). The
  Thumb-2 asm needs `-mimplicit-it=always` (`-Wa,` for gcc). Sync also lists `.cpp` now (`addtf3.cpp` is new).
- `arm/chkstk.S` (Windows `__chkstk`) never assembled with gcc for Thumb-2: excluded for all CPUs.
- **The optimized soft-float needs `-DCOMPILER_RT_ARMHF_TARGET` on a hard-float core** (found on the board 2026-10-03,
  in compiler-rt.cmake since): `cmpdf2.S` and friends pick their ABI from `__ARM_PCS_VFP`, `aeabi_dcmp.S`/`aeabi_fcmp.S`
  and the C files (`int_lib.h` COMPILER_RT_ABI) from that define, which upstream's builtins/CMakeLists.txt adds for an
  armhf target. Without it every `__aeabi_dcmp*` on the RP2350 compared d0/d1 garbage. Checked by test_examples
  `93_soft_float_check` (fails 360 compares without it). After a port: run 93 on an M33 and an M0+ board.

## Flags the libraries get from Kvasir (not from the port)

- 2026-10-05 (kvasir_work plans/binary_quality): `libc.cmake` builds the libc with `-fno-stack-protector` and its
  memory functions (`memcpy memmove mempcpy memset memset_explicit bcopy bzero`) at `-Os`; `libc-config/config.json`
  has `LIBC_CONF_ENABLE_STRONG_STACK_PROTECTOR` false to agree. The `.cmake` files are Kvasir's and survive a port;
  after one, check that those seven sources still exist under the same names (the `-Os` list names them).
- `lib/compiler-rt/builtins/kvasir/` (Kvasir-owned, `kvasir/compiler-rt/owned.txt`): `udivmoddi4_udiv.c`, the 64-bit
  division for cores with `UDIV`; host test in `Kvasir_SDK/tests/udivmoddi4/`.
- `lib/libc/kvasir/` (Kvasir-owned, `kvasir/libc/owned.txt`): `arm/memory_v6m.S`, memcpy/memmove/memset and the
  `__aeabi_mem*` entry points for the Cortex-M0+ (2026-10-06); `libc.cmake` drops upstream's three `.cpp` for v6-M and
  `compiler-rt.cmake` upstream's `aeabi_mem{cpy,move,set}.S`. Checked on the device by test_examples
  110_memory_check; after a port, run that on an M0+ board.
- 2026-10-06 (same plan): application code is built hosted for every core (`arm_clang.cmake` removes
  `-ffreestanding`), so the three libraries now carry `-ffreestanding` (libc and compiler-rt also `-fno-builtin`) in
  their own flag lists in `libc.cmake`, `compiler-rt.cmake`, `libcxx.cmake`. They must keep it: a hosted memcpy.cpp
  becomes a call to itself. `lib/libc/kvasir/arm/memory_v7m.S` (same day) is the Thumb-2 twin of memory_v6m.S;
  `libc.cmake` drops upstream's three `.cpp` for every core and `compiler-rt.cmake` the `aeabi_mem*.S` for every
  core. After a port: 110_memory_check on an M0+ AND an M33 board.
- **errno, strsignal and the allocator in `libc.cmake`** (2026-10-08, kvasir_work plans/atfe_ideas/08): the source
  list has no dependency edges and LTO hides a missing object until the first caller. `src/errno/libc_errno.cpp` is
  built (strtoimax, strtoumax, strdup, nan/nanf and every `errno` read need it); `src/string/strsignal.cpp` is
  commented out and in `kvasir/libc-config/exclude.txt` (its `signal_to_string.cpp` needs `<signal.h>`, which this
  libc has none of); `baremetal/free.cpp` and `aligned_alloc.cpp` sit in `LIBC_MALLOC_SOURCE_FILES` with malloc (their
  asserts need `exit.cpp`/`io.cpp`). After a port: free/aligned_alloc still in the heap list and not re-added to the
  main list as `# NEW`, strsignal still commented - then step 8.
- **compiler-rt's bare-metal profile runtime** (2026-10-08, kvasir_work plans/atfe_ideas/04): libcport mirrors
  `compiler-rt/lib/profile` and `include/profile` too (pruned to what `COMPILER_RT_PROFILE_BAREMETAL` builds), and
  syncs `lib/compiler-rt/profile.cmake` (Kvasir's: the list, `-DCOMPILER_RT_PROFILE_BAREMETAL=1 -ffreestanding
  -fno-builtin` and the exact `-Wno-*` set the files raise). Only `coverage` images link it. After a port: the raw
  profile format version may change - `Kvasir_SDK/tests/coverage` (host) and test_examples 115_coverage on a board
  (`kvasir_bench.py coverage`; llvm-profdata refusing the file is the failure).
