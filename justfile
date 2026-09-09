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
    libcxx="-DUSE_STD_LIB=libc++"
    case "{{config}}" in
        gcc)        echo "$gcc" ;;
        gcc-asan)   echo "$gcc -DUSE_SANITIZER=address -DUSE_STDLIB_DEBUG=true" ;;
        gcc-ubsan)  echo "$gcc -DUSE_SANITIZER=undefined" ;;
        clang)      echo "$clang" ;;
        clang-asan) echo "$clang $libcxx -DUSE_SANITIZER=address -DUSE_STDLIB_DEBUG=true" ;;
        clang-ubsan) echo "$clang $libcxx -DUSE_SANITIZER=undefined" ;;
        tsan)       echo "$clang $libcxx -DUSE_SANITIZER=thread" ;;
        *)
            echo "unknown config '{{config}}'; known: {{configs}}" >&2
            exit 1 ;;
    esac

# Wipe a build tree whose recorded source directory is not this one any more -
# what a moved checkout leaves behind. Without this CMake refuses outright:
# "the current CMakeCache.txt directory ... is different than the directory".
[private]
guard dir src:
    #!/usr/bin/env bash
    set -euo pipefail
    cache="{{ dir }}/CMakeCache.txt"
    [ -f "$cache" ] || exit 0
    home=$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$cache")
    if [ "$home" != "{{ src }}" ]; then
        echo "==> wiping {{ dir }}: source moved ($home)"
        rm -rf "{{ dir }}"
    fi

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

# Build and run the tests; FILTER is a ctest -R regex (e.g. "queue", "register")
test config=default_config filter="": (build config)
    ctest --test-dir "{{build_root}}/{{config}}" --output-on-failure -j{{jobs}} \
        {{ if filter == "" { "" } else { "-R '" + filter + "'" } }}

# Rebuild one test binary and run it directly (short name is fine: "spinlock")
run name config=default_config *args="": (configure config)
    #!/usr/bin/env bash
    set -euo pipefail
    target="{{name}}"
    case "$target" in kvasir_test_*) ;; *) target="kvasir_test_$target" ;; esac
    dir="{{build_root}}/{{config}}"
    cmake --build "$dir" -j{{jobs}} --target "$target"
    exec "$dir/$target" {{args}}

# Only the header self-containment check
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

# Remove every native build directory, including the pre-justfile tests/build*
clean-all:
    rm -rf "{{build_root}}" "{{ root }}/tests/build" "{{ root }}/tests/build_tsan"

# ---------------------------------------------------------------------------
# CI through act. Containers are reused (--reuse) so the pacman -Syu and the
# build tree survive between runs; images are only pulled when missing.
#
#   just ci                          # whole build-and-test matrix
#   just ci clang                    # every clang entry
#   just ci gcc address              # exactly the gcc + asan entry
#   just ci "" undefined             # both ubsan entries

act_job := "build-and-test"

# Run the CI test job through act; optional matrix filters on cc and sanitizer
ci cc="" sanitizer="":
    #!/usr/bin/env bash
    set -euo pipefail
    docker image inspect archlinux:latest >/dev/null 2>&1 || docker pull archlinux:latest
    args=(push -j {{act_job}} --reuse --pull=false)
    [ -n "{{cc}}" ] && args+=(--matrix "compiler.cc:{{cc}}")
    [ -n "{{sanitizer}}" ] && args+=(--matrix "compiler.sanitizer:{{sanitizer}}")
    act "${args[@]}"

# Show the act jobs/matrix entries without running anything
ci-list:
    act push -j {{act_job}} --list

# Remove the reused act containers (next `just ci` starts from a fresh image)
ci-clean:
    #!/usr/bin/env bash
    set -euo pipefail
    ids="$(docker ps -aq --filter name=act-)"
    if [ -n "$ids" ]; then
        docker rm -f $ids
    else
        echo "no act containers"
    fi
