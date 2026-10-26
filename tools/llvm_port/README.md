# llvm_port: how Kvasir_SDK's libc, libc++ and compiler-rt are made

`libcport` turns an llvm-project checkout into `lib/{libc,libcxx,compiler-rt}` of this SDK (each its own
submodule). Every change Kvasir makes to those three submodules must be recorded here - a patch, an owned file, an
excluded target or a config option - or the next port drops it. Paths below are relative to this directory: the SDK is
`../..`.

## What lives here

| path | what |
|---|---|
| `libcport` | the tool (`--help`). Port mode mirrors upstream, applies the patches, syncs the `.cmake` source lists and writes `<lib>/UPSTREAM`. `--save-patches` goes the other way. `--check` says whether a port at `UPSTREAM`'s tag would change anything. `--clear-new-marks` strips the `# NEW` marks of the source-list sync. |
| `libcport_lib.py` | libcport's Python: one subcommand per step (source-list sync, the libc_flags check, OPTIONS.md, the derived libc config, the owned.txt matcher, patch headers). Unit tests on fixtures: `tests/test_libcport_lib.py` (no llvm-project needed). |
| `fetch-llvm` | an llvm-project at a tag (default: `lib/libc/UPSTREAM`'s): a sparse, blobless clone, cached, its path printed. |
| `tests/check_selftest.sh <llvm>` | `--check` on a clean copy and against eight kinds of seeded drift in a scratch copy of the submodules and of `kvasir/` (seconds). |
| `tests/test_clear_new_marks.py` | `--clear-new-marks` over every source list of the component table. |
| `kvasir/` | **all of Kvasir's port state** (`KVASIR_STATE_DIR` moves it; the self-test points it at a scratch copy): |
| `kvasir/<lib>/patches/<path>.patch` | one patch per upstream file Kvasir changes (GNU patch, fuzz 2), each starting with its header block (`Kvasir patch:`, `Why:`, `Upstream:`, `Proof:`, `Drop when:`; format in `libcport --help`). `--save-patches` keeps the blocks and gives a new patch one full of TODOs, which `--check` refuses (NOHEADER). |
| `kvasir/<lib>/owned.txt` | Kvasir-only files: never overwritten, never deleted by the mirror (rsync glob meaning: `*` within a path component, `**` across, a trailing `/` a whole directory). |
| `kvasir/libc-config/exclude.txt` | regexes of libc targets taken OUT of upstream's `libc/config/baremetal/arm/{entrypoints,headers}.txt` (the lists are derived anew each port). |
| `kvasir/libc-config/config.json` | our `LIBC_CONF_*` values. They drive only the llvm-side headers build; Kvasir compiles the copied sources with `libc_flags` in `lib/libc.cmake`, and the port warns where the two disagree. |
| `kvasir/libc-config/OPTIONS.md` | every option upstream knows, its default and ours: regenerated each port, reference only. |

## Porting a new release

0. **Drift check first:** `./libcport --check $(./fetch-llvm) ../..`. Exit 0 = the submodules are exactly upstream
   + patches + owned files, so the port loses nothing; each finding says what to do (EDIT -> `--save-patches`,
   UNOWNED -> owned.txt, MISSING, REJECT, DEAD, NOHEADER, TAG). It reads the working tree, untracked owned
   directories included - unlike the port's own refusal. In full it builds the libc headers in a temp dir (about a
   minute); `--skip-libc-headers` takes seconds but leaves libc's `include/` unchecked - not enough before a port.
   Needs cmake, make, clang and PyYAML (libc's hdrgen: `ModuleNotFoundError: No module named 'yaml'` otherwise).
   Then triage: `git -C <llvm-project> diff --stat <old tag> <new tag> -- libc libcxx compiler-rt` shows what can
   reach us. libc math fixes inside `#ifndef LIBC_MATH_HAS_SKIP_ACCURATE_PASS` do not (config.json sets
   SKIP_ACCURATE_PASS); compiler-rt files absent from `lib/compiler-rt` do not either.
1. **Submodules committed and clean** (the port refuses otherwise). `--save-patches` first if anyone edited an
   upstream file since the last port (llvm-project at the tag in `<lib>/UPSTREAM`). The refusal counts UNTRACKED
   files too (Kvasir-owned `kvasir/` directories not yet committed), and it comes only after the libc build: check
   `git -C ../../lib/<lib> status --porcelain` is empty for all three first. Uncommitted work that must not be
   committed yet: `git stash push -u` in each, port, `git stash pop`.
   **`--save-patches --libc` needs the generated libc headers in `LIBC_BUILD_DIR`** (default `<llvm>/build`): it
   rewrites `kvasir/libc/patches/` from scratch, so without them the `include/*.h` patches would go. With no port
   build dir: config.json and the derived entrypoint/header lists into `<dir>/kvasir-libc-config` (libcport's
   `derive_libc_config`), its `configure_libc` cmake line, `--target generate-libc-headers`, then
   `LIBC_BUILD_DIR=<dir> ./libcport --save-patches --libc ...`. `git diff` the patches after: the old ones must come
   out unchanged.
2. **An llvm-project at the new tag:** `./fetch-llvm llvmorg-X` (sparse, blobless; enough for the port: `runtimes/`
   configures libc from it).
3. **`./libcport <llvm-project> ../..`**, the whole port with its libc build (`LIBC_BUILD_DIR` in a temp dir keeps
   the clone clean). Read the summary at the end.
4. A patch that did not apply leaves a `.rej`: check whether upstream fixed it itself (then drop it), else merge by
   hand; delete the `.rej` files; `./libcport --save-patches ...` to rewrite the patch set from the result. `patch -N`
   reports a patch upstream took over as "Reversed (or previously applied)".
5. Review the `# NEW` entries in libc.cmake, libcxx.cmake, compiler-rt.cmake and profile.cmake, enable what fits (libc:
   float/double math, ctype, string(s), stdlib basics; no f16/bf16/f128/long double), then
   `./libcport --clear-new-marks ../..`. "ACTIVE source(s) gone upstream" = look for what replaced them (math helpers
   that became header-only, for one). A new source list goes into libcport's component table (`source_lists`),
   nowhere else.
6. Compare `libcxx/include/__config_site.in` and `vendor/llvm/default_assertion_handler.in` across the two tags:
   Kvasir's `__config_site` is empty, the values are `libcxx_profile_flags` in `cmake/arm_compiler_common.cmake`.
7. Check every selected source compiles, clang AND gcc, Cortex-M0+ and M33, all variants, before any firmware build.
8. Run the SDK's checks of what a port can break, on an M33 and an M0+ target, clang and gcc, and on a board: the
   libc link check (links every enabled libc entrypoint, `cmake/tools/libc_link_probe.py` from `libc.cmake`; an
   undefined symbol is a source switched on without the object it needs), the soft-float compare check, the memory
   function check and the coverage round trip (see "What a port has met" for why each).

## What a port has met

- An option upstream removed (`LIBC_CONF_TIME_64BIT`: time_t is always 64-bit now) fails cmake; the port refuses it
  up front, while writing OPTIONS.md.
- libc's stdio entrypoints depend on `stdin/stdout/stderr` entries: a hand-copied entrypoints.txt failed configure.
  Hence the derived lists + exclude.txt.
- Generated headers' include guards became `_LLVM_LIBC_*`: patches anchored on the last line fail on the context.
- Upstream fixed four Kvasir patches itself (`vsqrt.f32/%P` for gcc, atan2f `MPI_OVER_2`, `stdfix-macros.h`'s
  `&& defined(__clang__)`, `limits_macros.h`: the full build uses libc's own `limits-macros.h`).
- The libc build dir holds `*.rsp` response files next to each header: pruned.
- libc++ removed its C wrappers (`ctype.h fenv.h float.h inttypes.h`); `stdbool.h` was gone already and gcc builds
  (`-nostdinc`) found it only in a stale copy - now a libc-owned `include/stdbool.h`.
- libc++ wraps `std` in a `#pragma clang attribute push` ABI region: its own sources warn
  (`-Wpragma-clang-attribute`, `-Wignored-attributes`) - suppressed in libcxx.cmake; user code sees system headers.
  gcc warns `-Wattributes` on the `always_inline` helpers of `src/include/to_chars_floating_point.h` (not `inline`).
- compiler-rt: `arm/addsf3.S` is the Thumb-2 optimized add; the Thumb-1 one is `arm/thumb1/addsf3.S`. Kvasir takes
  upstream's optimized soft-float (`COMPILER_RT_ARM_OPTIMIZED_FP`): one set per instruction set, lists in
  compiler-rt.cmake; the superseded C files include `arm/aeabi_frsub.c`/`aeabi_drsub.c` (found by a duplicate-symbol
  check: compile the selected files per core, `llvm-nm --defined-only -g`, `uniq -d`). The Thumb-2 assembly needs
  `-mimplicit-it=always` (`-Wa,` for gcc). The sync lists `.cpp` too (`addtf3.cpp`).
- `arm/chkstk.S` (Windows `__chkstk`) never assembles with gcc for Thumb-2: excluded for all CPUs.
- **The optimized soft-float needs `-DCOMPILER_RT_ARMHF_TARGET` on a hard-float core** (in compiler-rt.cmake):
  `cmpdf2.S` and friends pick their ABI from `__ARM_PCS_VFP`, `aeabi_dcmp.S`/`aeabi_fcmp.S` and the C files
  (`int_lib.h` COMPILER_RT_ABI) from that define, which upstream's builtins/CMakeLists.txt adds for an armhf target.
  Without it every `__aeabi_dcmp*` on a Cortex-M33 compared d0/d1 garbage. The soft-float compare check fails
  hundreds of compares without it: after a port, run it on an M33 and an M0+.

## Flags the libraries get from Kvasir (not from the port)

The `.cmake` files are Kvasir's and survive a port; after one, check that what they name still exists.

- The three libraries compile with the compiler's default warnings: Kvasir's warning list (`cmake/kvasir_warnings.cmake`,
  the target `kvasir::warnings`) is the image's own code's, not in `CMAKE_<LANG>_FLAGS`. Their lists carry only the
  `-Wno-*` a default warning needs (libc++: the two above); a port that brings a new default warning adds its `-Wno-*`
  there, per compiler.

- `libc.cmake` builds the libc with `-fno-stack-protector` and its memory functions (`memcpy memmove mempcpy memset
  memset_explicit bcopy bzero`) at `-Os`; `kvasir/libc-config/config.json` has
  `LIBC_CONF_ENABLE_STRONG_STACK_PROTECTOR` false to agree. After a port: those seven sources under the same names.
- `lib/compiler-rt/builtins/kvasir/` (owned, `kvasir/compiler-rt/owned.txt`): `udivmoddi4_udiv.c`, the 64-bit
  division for cores with `UDIV`; host test in `tests/udivmoddi4/`.
- `lib/libc/kvasir/` (owned, `kvasir/libc/owned.txt`): `arm/memory_v6m.S` and `arm/memory_v7m.S`,
  memcpy/memmove/memset and the `__aeabi_mem*` entry points for the Cortex-M0+ and Thumb-2; `libc.cmake` drops
  upstream's three `.cpp` and `compiler-rt.cmake` upstream's `aeabi_mem{cpy,move,set}.S`. After a port: the memory
  function check on an M0+ AND an M33.
- Application code is built hosted for every core (`arm_clang.cmake` removes `-ffreestanding`), so the three libraries
  carry `-ffreestanding` (libc and compiler-rt also `-fno-builtin`) in their own flag lists in `libc.cmake`,
  `compiler-rt.cmake`, `libcxx.cmake`. They must keep it: a hosted memcpy.cpp becomes a call to itself.
- **errno, strsignal and the allocator in `libc.cmake`:** the source list has no dependency edges and LTO hides a
  missing object until the first caller. `src/errno/libc_errno.cpp` is built (strtoimax, strtoumax, strdup,
  nan/nanf and every `errno` read need it); `src/string/strsignal.cpp` is commented out and in
  `kvasir/libc-config/exclude.txt` (its `signal_to_string.cpp` needs `<signal.h>`, which this libc has none of);
  `baremetal/free.cpp` and `aligned_alloc.cpp` sit in `LIBC_MALLOC_SOURCE_FILES` with malloc (their asserts need
  `exit.cpp`/`io.cpp`). After a port: free/aligned_alloc still in the heap list and not re-added to the main list as
  `# NEW`, strsignal still commented - then the libc link check.
- **compiler-rt's bare-metal profile runtime:** libcport mirrors `compiler-rt/lib/profile` and `include/profile` too
  (pruned to what `COMPILER_RT_PROFILE_BAREMETAL` builds) and syncs `lib/compiler-rt/profile.cmake` (Kvasir's: the
  list, `-DCOMPILER_RT_PROFILE_BAREMETAL=1 -ffreestanding -fno-builtin` and the exact `-Wno-*` set the files raise).
  Only `coverage` images link it. After a port the raw profile format version may change: `tests/coverage` (host)
  and a coverage image on a board (`kvasir_bench.py coverage`; llvm-profdata refusing the file is the failure).
