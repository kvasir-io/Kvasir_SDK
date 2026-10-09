#!/usr/bin/env bash
#
# libcport --check against seeded drift: a scratch copy of <kvasir-sdk>/lib/{libc,libcxx,compiler-rt} and of this
# tool's kvasir/ state, one change per scenario, and the finding line and exit code each must give.
#
#   tests/check_selftest.sh <llvm-project at the lib/*/UPSTREAM tag> [<kvasir-sdk>]
#
# Exit 77 (skipped) without an llvm-project checkout. The real lib/ is only read. Uses --skip-libc-headers (no
# cmake; seconds): the generated libc headers are what `libcport --check` without it covers.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PORT="$HERE/../libcport"
LLVM="${1:-}"
SDK="${2:-$HERE/../../..}"
[ -n "$LLVM" ] && [ -d "$LLVM/libc" ] || {
    echo "skipped: no llvm-project checkout given"
    exit 77
}

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
failed=0

# a fresh scratch copy for each scenario: <work>/sdk/lib/<lib> and <work>/state (KVASIR_STATE_DIR)
fresh() {
    rm -rf "$WORK/sdk" "$WORK/state"
    mkdir -p "$WORK/sdk/lib"
    for lib in libc libcxx compiler-rt; do
        rsync -a --exclude=.git "$SDK/lib/$lib/" "$WORK/sdk/lib/$lib/"
    done
    cp -r "$HERE/../kvasir" "$WORK/state"
}

# scenario <name> <expected exit> <expected line regex or ''> -- runs the check on the current scratch copy
expect() {
    local name="$1" rc_want="$2" want="$3" out rc
    out="$(KVASIR_STATE_DIR="$WORK/state" NO_COLOR=1 "$PORT" --check --skip-libc-headers "$LLVM" "$WORK/sdk" 2>&1)"
    rc=$?
    if [ "$rc" -ne "$rc_want" ] || { [ -n "$want" ] && ! grep -qE "$want" <<<"$out"; }; then
        echo "FAIL $name: exit $rc (want $rc_want), want a line matching '$want'"
        sed 's/^/     /' <<<"$out" | grep -E '^ +(EDIT|UNOWNED|MISSING|REJECT|DEAD|NOHEADER|TAG|clean)' | head -20
        failed=$((failed + 1))
    else
        echo "ok   $name"
    fi
}

fresh
expect "clean tree" 0 'clean: a port at'

fresh
echo '// drift' >>"$WORK/sdk/lib/libcxx/include/__config"
expect "edited upstream file" 1 '^   EDIT     include/__config$'

fresh
echo 'int foo;' >"$WORK/sdk/lib/libc/src/foo.cpp"
expect "unowned new file" 1 '^   UNOWNED  src/foo.cpp$'

fresh
rm "$WORK/sdk/lib/compiler-rt/builtins/udivsi3.c"
expect "deleted upstream file" 1 '^   MISSING  builtins/udivsi3.c$'

fresh
sed -i 's/^ #else$/ #else  \/\/ context no longer matches/' "$WORK/state/compiler-rt/patches/builtins/int_lib.h.patch"
expect "patch that no longer applies" 1 '^   REJECT   builtins/int_lib.h.patch$'

fresh
echo 'src/no_such_dir/' >>"$WORK/state/libcxx/owned.txt"
expect "owned.txt line matching nothing" 1 '^   DEAD     owned.txt: src/no_such_dir/$'

fresh
sed -i '/^Proof: /d' "$WORK/state/libcxx/patches/include/__verbose_abort.patch"
expect "patch without a complete header" 1 '^   NOHEADER include/__verbose_abort.patch$'

fresh
sed -i 's/^Why: .*/Why:       TODO/' "$WORK/state/libcxx/patches/include/__verbose_abort.patch"
expect "patch header with a TODO" 1 '^   NOHEADER include/__verbose_abort.patch \(a TODO'

fresh
sed -i '1s/^[^ ]*/llvmorg-0.0.0/' "$WORK/sdk/lib/compiler-rt/UPSTREAM"
expect "UPSTREAM at another tag" 1 '^   TAG '

echo
[ "$failed" -eq 0 ] && echo "all scenarios pass" || echo "$failed scenario(s) failed"
[ "$failed" -eq 0 ]
