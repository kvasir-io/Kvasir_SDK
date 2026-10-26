# The shared upkeep recipes of every chip package. A chip's justfile defines `root`, `chip`, `docker_args` and `sdk`
# and then says `import? '../Kvasir_SDK/just/chip.just'`; its own `default` recipe says what the package is.
#
# Never define `root` (or call source_directory()) here: in an imported file source_directory() is THIS file's
# directory (Kvasir_SDK/just), so every recipe would point at the SDK instead of the chip package.

# `git submodule update` discards uncommitted work inside a populated submodule,
# and this tree has some, so only missing ones are touched.
# Initialise any submodule that is not checked out yet, core/'s own included
submodules:
    #!/usr/bin/env bash
    set -euo pipefail
    root="{{ root }}"
    # `git submodule status` cannot read a repository while one of its submodule
    # paths is a symlink (the workspace's link step makes them), so walk
    # .gitmodules directly: a submodule counts as missing when its path is absent
    # or empty. This repository first, so a fresh core/ is there to walk next.
    initialised=0
    for repo in "$root" "$root/core"; do
        [ -f "$repo/.gitmodules" ] || continue
        missing=()
        while read -r _ path; do
            full="$repo/$path"
            # a symlinked path is deliberate - never re-initialise over it
            if [ -L "$full" ]; then
                continue
            fi
            if [ ! -d "$full" ] || [ -z "$(ls -A "$full" 2>/dev/null)" ]; then
                missing+=("$path")
            fi
        done < <(git -C "$repo" config -f "$repo/.gitmodules" --get-regexp '^submodule\..*\.path$')
        [ ${#missing[@]} -ne 0 ] || continue
        printf 'initialising in %s: %s\n' "$repo" "${missing[*]}"
        git -C "$repo" submodule update --init --recursive -- "${missing[@]}"
        initialised=1
    done
    [ $initialised -eq 1 ] || echo "all submodules present"

# No .clang-format is checked in, so this follows ~/.clang-format - the same
# config svd_converter formats its generated headers with.
# List the sources clang-format would rewrite
format-check:
    #!/usr/bin/env bash
    set -euo pipefail
    mapfile -t files < <(find "{{ root }}/src" "{{ root }}/core/src" \
        \( -name '*.hpp' -o -name '*.cpp' \) -print 2>/dev/null | sort)
    [ ${#files[@]} -ne 0 ] || { echo "no sources found"; exit 0; }
    dirty=()
    for f in "${files[@]}"; do
        clang-format --output-replacements-xml "$f" | grep -q '<replacement ' && dirty+=("$f")
    done
    if [ ${#dirty[@]} -eq 0 ]; then
        echo "${#files[@]} files, all formatted"
        exit 0
    fi
    printf 'would reformat (%d of %d):\n' "${#dirty[@]}" "${#files[@]}"
    printf '  %s\n' "${dirty[@]#{{ root }}/}"
    exit 1

# Rewrite those sources in place. Nothing else calls this - the diff is yours.
format:
    #!/usr/bin/env bash
    set -euo pipefail
    mapfile -t files < <(find "{{ root }}/src" "{{ root }}/core/src" \
        \( -name '*.hpp' -o -name '*.cpp' \) -print 2>/dev/null | sort)
    [ ${#files[@]} -ne 0 ] || { echo "no sources found"; exit 0; }
    clang-format -i "${files[@]}"
    echo "formatted ${#files[@]} files"

# The image the chip's <chip>-docker-build.yml publishes: the SDK's scripts/Dockerfile.chip with that workflow's
# arguments (`docker_args`); `sdk` is the Kvasir_SDK checkout next to the chip (`just sdk=<dir> docker` for another).
# Build the container image CI publishes as kvasirio/<chip>:latest
docker tag=("kvasirio/" + chip + ":latest"):
    docker build -f "{{ sdk }}/scripts/Dockerfile.chip" {{ docker_args }} -t "{{ tag }}" "{{ root }}"

# The SDK's error classes (Runtime/Check.hpp: UC_LOG_C only after Panic::beginFatal()); scanner from `sdk`.
# Run the SDK's fatal-path and log-level rules over this package's sources
lint:
    python3 -B {{ sdk }}/tools/check_fatal_sites.py {{ root }}/src

# The conformance tree (tests/conformance/build: the contract rows, the drivers' instantiations and every public
# header alone, for the chip's target; configured with clang and the default variant on first use; a chip whose tree
# is its common package's - chip_rp2040 and chip_rp2350: src/chip/rp_common/tests/conformance, built in its
# build/<chip> with -DCONFORMANCE_CHIP=<chip>) through
# `sdk`'s tools/tidy.py and .clang-tidy: findings in this package fail, the SDK's and kvasir_devices' count as
# foreign. `just tidy --changed`, `just tidy --fix`, `--audit <check>`, `--only '<glob>'`.
# clang-tidy over the chip's conformance tree (exit 0 clean, 1 findings)
[positional-arguments]
tidy *args:
    #!/usr/bin/env bash
    set -euo pipefail
    src="{{ root }}/tests/conformance"
    dir="$src/build"
    args=()
    if [ ! -d "$src" ] && [ -d "{{ root }}/src/chip/rp_common/tests/conformance" ]; then
        src="{{ root }}/src/chip/rp_common/tests/conformance"
        dir="$src/build/{{ chip }}"
        args=(-DCONFORMANCE_CHIP={{ chip }})
    fi
    if [ ! -f "$dir/compile_commands.json" ]; then
        cmake -S "$src" -B "$dir" -G Ninja "${args[@]}" > /dev/null
    fi
    exec python3 -B "{{ sdk }}/tools/tidy.py" --tree "$dir" --repo "{{ root }}" "$@"

# The configured conformance tree `tidy` reads, if there is one (the workspace's `just tidy` collects them)
tidy-trees:
    #!/usr/bin/env bash
    set -euo pipefail
    dir="{{ root }}/tests/conformance/build"
    [ -d "{{ root }}/tests/conformance" ] || dir="{{ root }}/src/chip/rp_common/tests/conformance/build/{{ chip }}"
    [ ! -f "$dir/compile_commands.json" ] || echo "$dir"
