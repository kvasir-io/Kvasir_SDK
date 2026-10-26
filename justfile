# Local development helpers for the Kvasir SDK host tests.
#
# Native builds live in build/<config>/ (one Ninja tree per configuration, so
# switching configs never throws away another config's objects). The configs
# mirror the CI matrix in .github/workflows/ci.yml, plus `tsan` for the
# multicore primitives.
#
#   just                    # build + run the tests with the default config
#   just test clang         # same, with clang
#   just test gcc-asan Queue    # only tests matching "Queue", asan config
#   just run spinlock tsan  # rebuild one test binary and run it directly
#   just test-all           # every config, like CI does
#   just ci                 # the real CI job through act, containers reused
#
# `just --list` shows everything.

set shell := ["bash", "-euo", "pipefail", "-c"]

# source_directory()/source_file(), not justfile_directory()/justfile(): those
# name the ROOT justfile when this one is loaded as a module from ../justfile.
root := source_directory()
self := source_file()

default_config := "gcc"
build_root := root / "build"
jobs := num_cpus()

# All configurations, in CI order. Used by test-all / build-all.
configs := "gcc gcc-asan gcc-ubsan clang clang-asan clang-ubsan tsan"

default: test

# Print the cmake flags for a configuration (see the CI matrix)
[private]
flags config:
    #!/usr/bin/env bash
    set -euo pipefail
    gcc="-DCMAKE_CXX_COMPILER=g++"
    clang="-DCMAKE_CXX_COMPILER=clang++"
    libcxx="-DKVASIR_TEST_STD_LIB=libc++"
    case "{{config}}" in
        gcc)        echo "$gcc" ;;
        gcc-asan)   echo "$gcc -DKVASIR_TEST_SANITIZER=address -DKVASIR_TEST_STDLIB_DEBUG=true" ;;
        gcc-ubsan)  echo "$gcc -DKVASIR_TEST_SANITIZER=undefined" ;;
        clang)      echo "$clang" ;;
        clang-asan) echo "$clang $libcxx -DKVASIR_TEST_SANITIZER=address -DKVASIR_TEST_STDLIB_DEBUG=true" ;;
        clang-ubsan) echo "$clang $libcxx -DKVASIR_TEST_SANITIZER=undefined" ;;
        tsan)       echo "$clang $libcxx -DKVASIR_TEST_SANITIZER=thread" ;;
        *)
            echo "unknown config '{{config}}'; known: {{configs}}" >&2
            exit 1 ;;
    esac

# Wipe a build tree the checkout outgrew: a moved source directory, a recorded
# *_ROOT checkout that is gone, another generator (every recipe here drives
# Ninja), or a configure that never finished. Without this CMake refuses
# outright: "the current CMakeCache.txt directory ... is different than the
# directory". tools/fw.py guard.
[private]
guard dir src:
    @python3 -B "{{ root }}/tools/fw.py" guard --tree "{{ dir }}" --source "{{ src }}"

# Configure build/<config> if it does not exist yet (no-op afterwards)
configure config=default_config: (guard (build_root / config) (root / "tests"))
    #!/usr/bin/env bash
    set -euo pipefail
    dir="{{build_root}}/{{config}}"
    if [ -f "$dir/build.ninja" ]; then
        exit 0
    fi
    flags="$(just --justfile "{{ self }}" flags "{{config}}")"
    launcher=""
    if command -v ccache >/dev/null; then
        launcher="-DCMAKE_CXX_COMPILER_LAUNCHER=ccache"
    fi
    cmake -S "{{ root }}/tests" -B "$dir" -G Ninja \
        -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        $launcher $flags

# Wipe build/<config> and configure it again
reconfigure config=default_config: (clean config) (configure config)

# Build all test binaries of a configuration (incremental)
build config=default_config: (configure config)
    cmake --build "{{build_root}}/{{config}}" -j{{jobs}}

# Build and run the tests; FILTER is a ctest -R regex (e.g. "queue", "register"), LABELS a ctest -L regex of the test
# kinds (unit, mock, threaded, compile, must_fail, tool, cross_repo; e.g. `just test clang "" threaded`)
test config=default_config filter="" labels="": (build config)
    ctest --test-dir "{{build_root}}/{{config}}" --output-on-failure -j{{jobs}} \
        {{ if filter == "" { "" } else { "-R '" + filter + "'" } }} \
        {{ if labels == "" { "" } else { "-L '" + labels + "'" } }}

# Rebuild one test binary and run it directly (short name is fine: "spinlock")
run name config=default_config *args="": (configure config)
    #!/usr/bin/env bash
    set -euo pipefail
    target="{{name}}"
    case "$target" in kvasir_test_*) ;; *) target="kvasir_test_$target" ;; esac
    dir="{{build_root}}/{{config}}"
    cmake --build "$dir" -j{{jobs}} --target "$target"
    exec "$dir/$target" {{args}}

# Only the header self-containment check (every header alone, on the register mock and on the volatile path)
headers config=default_config: (configure config)
    cmake --build "{{build_root}}/{{config}}" -j{{jobs}} --target kvasir_self_contained_headers

# Rerun only the tests that failed last time
retest config=default_config:
    ctest --test-dir "{{build_root}}/{{config}}" --output-on-failure -j{{jobs}} --rerun-failed

# List the tests of a configuration
list config=default_config: (configure config)
    ctest --test-dir "{{build_root}}/{{config}}" -N

# Build every configuration
build-all:
    #!/usr/bin/env bash
    set -euo pipefail
    for c in {{configs}}; do
        echo "==> build $c"
        just --justfile "{{ self }}" build "$c"
    done

# Run the tests in every configuration, like CI (continues after failures)
test-all filter="":
    #!/usr/bin/env bash
    set -uo pipefail
    failed=()
    for c in {{configs}}; do
        echo "==> test $c"
        just --justfile "{{ self }}" test "$c" "{{filter}}" || failed+=("$c")
    done
    if [ ${#failed[@]} -ne 0 ]; then
        echo "FAILED: ${failed[*]}" >&2
        exit 1
    fi
    echo "all configurations passed"

# Remove build/<config>
clean config=default_config:
    rm -rf "{{build_root}}/{{config}}"

# Remove every native build directory
clean-all:
    rm -rf "{{build_root}}"

# ---------------------------------------------------------------------------
# CI through act. Containers are reused (--reuse) so the pacman -Syu and the
# build tree survive between runs; images are only pulled when missing.
#
#   just ci                          # whole build-and-test matrix
#   just ci clang                    # every clang entry
#   just ci gcc address              # exactly the gcc + asan entry
#   just ci "" undefined             # both ubsan entries
#
# act's --matrix cannot pick from this workflow's object entries (`compiler: {cc: ...}`: it matches none and runs
# all), so a filter runs a copy of ci.yml with only the matching entries, named after the filter (act names its
# containers by workflow and job: each filter reuses its own).

act_job := "build-and-test"

# Run the CI test job through act; optional matrix filters on cc and sanitizer
ci cc="" sanitizer="":
    #!/usr/bin/env bash
    set -euo pipefail
    docker image inspect archlinux:latest >/dev/null 2>&1 || docker pull archlinux:latest
    if [ -z "{{cc}}{{sanitizer}}" ]; then
        exec act push -j {{act_job}} --reuse --pull=false
    fi
    dir=$(mktemp -d)
    trap 'rm -rf "$dir"' EXIT
    python3 - .github/workflows/ci.yml "$dir/ci.yml" "{{cc}}" "{{sanitizer}}" <<'PY'
    import re, sys
    src, dst, cc, san = sys.argv[1:]
    kept, out = 0, []
    for line in open(src):
        entry = re.match(r'\s*- \{(.*)\}\s*$', line)
        if entry and re.search(r'\bcc:', entry.group(1)):
            keys = dict(kv.split(':', 1) for kv in entry.group(1).replace(' ', '').split(','))
            if (cc and keys.get('cc') != cc) or (san and keys.get('sanitizer') != san):
                continue
            kept += 1
        out.append(re.sub(r'^name: .*', f'name: CI {cc or "any"} {san or "any"}', line))
    if kept == 0:
        sys.exit(f'just ci: no matrix entry of {src} has cc={cc or "*"} sanitizer={san or "*"}')
    open(dst, 'w').writelines(out)
    print(f'just ci: {kept} matrix entries')
    PY
    act workflow_dispatch -j {{act_job}} -W "$dir/ci.yml" --reuse --pull=false

# Show the act jobs/matrix entries without running anything
ci-list:
    act push -j {{act_job}} --list

# Remove this repository's reused act containers and their volumes (next `just ci` starts from a fresh image). Only
# those: act names containers by workflow and job ("act-CI-build-and-test-..."), alike for every repository, but mounts
# its copy of the repository (a volume) at the repository's own path - that path tells whose container it is.
ci-clean:
    #!/usr/bin/env bash
    set -euo pipefail
    here="$(realpath "{{ root }}")"
    ids=()
    for id in $(docker ps -aq --filter name=act-); do
        if docker inspect -f '{{ "{{" }}range .Mounts{{ "}}" }}{{ "{{" }}.Destination{{ "}}" }} {{ "{{" }}end{{ "}}" }}' "$id" | tr ' ' '\n' | grep -qxF "$here"; then
            ids+=("$id")
        fi
    done
    if [ ${#ids[@]} -eq 0 ]; then
        echo "no act containers of $here"
        exit 0
    fi
    vols=()
    for id in "${ids[@]}"; do
        name="$(docker inspect -f '{{ "{{" }}.Name{{ "}}" }}' "$id")"
        vols+=("${name#/}" "${name#/}-env")
    done
    docker rm -f "${ids[@]}"
    docker volume rm "${vols[@]}" 2>/dev/null || true

# --- clang-tidy ----------------------------------------------------------------

# Kvasir_SDK/tools/tidy.py with the SDK's .clang-tidy over build/clang (the tests, the self-contained headers on the volatile path, uc_log and svd_converter as the tests
# compile them; the nested checkouts count as this repository): findings in this repository fail, the
# other libraries' count as foreign. `just tidy --changed`, `just tidy --fix`, `--audit <check>`, `--only '<glob>'`.
# clang-tidy over the clang host tree (exit 0 clean, 1 findings)
[positional-arguments]
tidy *args: (configure "clang")
    python3 -B "{{ root }}/tools/tidy.py" --tree "{{ build_root }}/clang" --repo "{{ root }}" --nested "$@"

# The configured clang tree `tidy` reads, if there is one (the workspace's `just tidy` collects them)
tidy-trees:
    @[ ! -f "{{ build_root }}/clang/compile_commands.json" ] || echo "{{ build_root }}/clang"
