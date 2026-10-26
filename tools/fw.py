#!/usr/bin/env python3
"""The firmware justfiles' shared logic: the build-tree guard, the build-all filter, the stale-tree scan and the
compiler/library mapping.

    fw.py guard       --tree DIR --source SRC [--expect VAR=PATH]...
    fw.py build-all   --tree DIR --ok MESSAGE [--] [TARGET...]
    fw.py stale-trees ROOT
    fw.py config      --compiler clang|gcc --libs llvm|newlib suffix|cmake-flags

guard: wipes DIR when its CMakeCache.txt no longer fits the checkout - the source directory moved, a recorded
dependency checkout (any `*_ROOT` cache entry but CMAKE_ROOT) is gone, an `--expect`ed entry has another value,
the configure never finished (no CMAKE_PROJECT_NAME: an interrupted configure leaves a cache and no build files),
or the tree was made by another generator than Ninja. The last reason found is the one printed. Without a cache
it does nothing.

build-all: `cmake --build DIR --target TARGET...` (no target: the whole tree). Silence is the pass condition: on
failure every failed ninja edge is printed whole (its first 40 lines, cut at 400 characters; the output's last 30
lines when nothing FAILED) and "BUILD FAILED" goes to stderr with cmake's exit code; a successful build that
printed a compiler diagnostic shows those lines numbered and exits 1 ("NOT CLEAN"); otherwise MESSAGE is printed.
No -j: ninja joins a make jobserver it inherits through MAKEFLAGS, so the environment and the file descriptors are
passed on unchanged.

stale-trees: prints "<dir>\\t<why>" for every outermost build tree below ROOT whose source or a recorded
dependency checkout is gone, whose configure never finished (no CMAKE_PROJECT_NAME), or that was made by another
generator. FetchContent `_deps` trees and `docker_build` (container paths) are skipped. A configure running right
now looks the same as an interrupted one: run the scan (and `repair`, which deletes what it lists) while no
configure runs below ROOT.

config: the build tree's suffix or the configure flags of one compiler/library pair. The suffix is "" for the
compiler's own libraries (clang + llvm, gcc + newlib) and "_newlib" / "_llvm" for the other ones, after "_gcc" for
gcc: build, build_newlib, build_gcc, build_gcc_llvm. cmake-flags are -DCOMPILE_TARGET=arm_gcc (gcc only) and the
-DCPPLIB/-DCLIB pair. Another compiler or library is an error naming the value (exit 1, nothing on stdout); both
are checked for either output.

Standard library only; run it with `python3 -B` (no __pycache__ next to the tools).
"""

import argparse
import os
import re
import shutil
import subprocess
import sys

DIAGNOSTIC = re.compile(
    r": (warning|error|fatal error):|^FAILED:|^ninja: (error|build stopped)")
EDGE_END = re.compile(r"^\[[0-9]+/[0-9]+\] |^ninja: ")
NINJA_STOP = re.compile(r"^ninja: (error|build stopped)")
ROOT_ENTRY = re.compile(r"^([A-Za-z0-9_]+_ROOT):[A-Za-z]*=(.*)$")

FAILED_HEAD = 200   # characters of a FAILED: line
EDGE_LINES = 40     # lines of a failed edge's output
EDGE_WIDTH = 400    # characters of each of them
TAIL_LINES = 30     # the output's tail when nothing FAILED


def read_cache(path):
    """The cache's lines, or None without one."""
    try:
        with open(path, encoding="utf-8", errors="surrogateescape") as f:
            return f.read().split("\n")
    except FileNotFoundError:
        return None


def entry(lines, prefix):
    """The value of the first line starting with prefix ('' if none)."""
    for line in lines:
        if line.startswith(prefix):
            return line[len(prefix):]
    return ""


def typed_entry(lines, name):
    """The value of the first `name:<type>=` line ('' if none)."""
    pattern = re.compile(r"^" + re.escape(name) + r":[A-Za-z]*=(.*)$")
    for line in lines:
        m = pattern.match(line)
        if m:
            return m.group(1)
    return ""


def root_entries(lines):
    """(name, value) of every `*_ROOT` entry but CMAKE_ROOT, the first line of each name, in cache order."""
    seen = {}
    for line in lines:
        m = ROOT_ENTRY.match(line)
        if m and m.group(1) != "CMAKE_ROOT" and m.group(1) not in seen:
            seen[m.group(1)] = m.group(2)
    return list(seen.items())


def dependency_reasons(lines):
    """The 'VAR gone (p)' reasons of the cache's dependency roots, in cache order."""
    return [f"{name} gone ({value})" for name, value in root_entries(lines) if value and not os.path.isdir(value)]


def generator_reason(lines):
    generator = entry(lines, "CMAKE_GENERATOR:INTERNAL=")
    return [] if generator == "Ninja" else [f"generator is '{generator}', not Ninja"]


def guard_reason(lines, source, expect):
    """The reason to wipe a tree with this cache (the last one found), or None."""
    why = []
    home = entry(lines, "CMAKE_HOME_DIRECTORY:INTERNAL=")
    if home != source:
        why.append(f"source moved ({home})")
    why += dependency_reasons(lines)
    for name, want in expect:
        have = typed_entry(lines, name)
        if have != want:
            why.append(f"{name} is {have}, not {want}")
    if not any(line.startswith("CMAKE_PROJECT_NAME:") for line in lines):
        why.append("configure never finished")
    why += generator_reason(lines)
    return why[-1] if why else None


def cmd_guard(args):
    expect = []
    for item in args.expect:
        name, sep, want = item.partition("=")
        if not sep or not name:
            sys.exit(f"fw.py guard: --expect takes VAR=PATH, got '{item}'")
        expect.append((name, want))
    lines = read_cache(os.path.join(args.tree, "CMakeCache.txt"))
    if lines is None:
        return 0
    why = guard_reason(lines, args.source, expect)
    if why:
        print(f"==> wiping {args.tree}: {why}", flush=True)
        shutil.rmtree(args.tree, ignore_errors=True)
    return 0


def failed_edges(lines):
    """The bash filter's awk: every FAILED edge whole, and ninja's own stop line."""
    shown = []
    on = False
    n = 0
    for line in lines:
        if line.startswith("FAILED:"):
            on = True
            n = 0
            shown.append(line[:FAILED_HEAD])
            continue
        if EDGE_END.search(line):
            on = False
        if on:
            n += 1
            if n <= EDGE_LINES:
                shown.append(line[:EDGE_WIDTH])
        if NINJA_STOP.search(line):
            shown.append(line)
    return shown


def run_build(tree, targets):
    """cmake --build with stdout and stderr in one stream, as bash's `$(... 2>&1)` sees it: (text, rc)."""
    cmd = ["cmake", "--build", tree]
    if targets:
        cmd += ["--target", *targets]
    try:
        proc = subprocess.run(cmd, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, close_fds=False)
    except FileNotFoundError:
        return "fw.py: cmake: command not found", 127
    rc = proc.returncode if proc.returncode >= 0 else 128 - proc.returncode
    # $(...) drops the trailing newlines (and NUL bytes)
    text = proc.stdout.replace(b"\0", b"").decode(
        "utf-8", errors="surrogateescape").rstrip("\n")
    return text, rc


def write(stream, text):
    stream.write(text.encode("utf-8", errors="surrogateescape"))
    stream.flush()


def filter_build(out, rc, ok):
    """The filter on a build's output: (stdout, stderr, exit code)."""
    lines = out.split("\n")   # not splitlines(): awk and grep split on \n only
    if rc != 0:
        shown = "\n".join(failed_edges(lines)).rstrip("\n")
        if shown:
            text = shown + "\n"
        else:
            text = "\n".join(lines[-TAIL_LINES:]) + "\n"
        return text, "BUILD FAILED\n", rc
    hits = [f"{i}:{line}" for i, line in enumerate(
        lines, 1) if DIAGNOSTIC.search(line)]
    if hits:
        return "\n".join(hits) + "\n", "NOT CLEAN - warnings above\n", 1
    return ok + "\n", "", 0


def cmd_build_all(args):
    targets = args.targets[1:] if args.targets[:1] == ["--"] else args.targets
    out, rc = run_build(args.tree, targets)
    stdout, stderr, code = filter_build(out, rc, args.ok)
    write(sys.stdout.buffer, stdout)
    write(sys.stderr.buffer, stderr)
    return code


def stale_reason(lines):
    """Why a tree found by the scan is stale (the last reason), or None."""
    why = []
    home = entry(lines, "CMAKE_HOME_DIRECTORY:INTERNAL=")
    if home and not os.path.isdir(home):
        why.append(f"source gone ({home})")
    why += dependency_reasons(lines)
    if not any(line.startswith("CMAKE_PROJECT_NAME:") for line in lines):
        why.append("configure never finished")
    why += generator_reason(lines)
    return why[-1] if why else None


def stale_trees(root):
    """(dir, why) of every stale outermost tree below root, shallowest first."""
    caches = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in (
            "_deps", "docker_build")]
        if "CMakeCache.txt" in filenames:
            caches.append(dirpath)
    caches.sort(key=lambda d: (d.count(os.sep), d))
    kept = []
    found = []
    for tree in caches:
        if any((tree + os.sep).startswith(k + os.sep) for k in kept):
            continue
        lines = read_cache(os.path.join(tree, "CMakeCache.txt"))
        if lines is None:      # TryCompile scratch can vanish mid-scan
            continue
        kept.append(tree)
        why = stale_reason(lines)
        if why:
            found.append((tree, why))
    return found


def cmd_stale_trees(args):
    for tree, why in stale_trees(os.path.normpath(args.root)):
        sys.stdout.write(f"{tree}\t{why}\n")
    return 0


COMPILERS = ("clang", "gcc")
LIBS = {"llvm": "-DCPPLIB=libc++ -DCLIB=llvm",
        "newlib": "-DCPPLIB=libstdc++ -DCLIB=newlib"}
OWN_LIBS = {"clang": "llvm", "gcc": "newlib"}


def config_value(compiler, libs, what):
    """The suffix or the cmake flags of a compiler/library pair; ValueError names a bad value."""
    if compiler not in COMPILERS:
        raise ValueError(f"compiler must be clang or gcc, got '{compiler}'")
    if libs not in LIBS:
        raise ValueError(f"libs must be llvm or newlib, got '{libs}'")
    if what == "suffix":
        return ("_gcc" if compiler == "gcc" else "") + ("" if libs == OWN_LIBS[compiler] else f"_{libs}")
    return ("-DCOMPILE_TARGET=arm_gcc " if compiler == "gcc" else "") + LIBS[libs]


def cmd_config(args):
    try:
        value = config_value(args.compiler, args.libs, args.what)
    except ValueError as e:
        sys.stderr.write(f"error: {e}\n")
        return 1
    sys.stdout.write(value)
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(
        prog="fw.py", description=__doc__.split("\n\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("guard", help="wipe a build tree the checkout outgrew")
    p.add_argument("--tree", required=True, help="the build tree")
    p.add_argument("--source", required=True,
                   help="the source directory it must be configured from")
    p.add_argument("--expect", action="append", default=[], metavar="VAR=PATH",
                   help="a cache entry that must have this value (repeatable)")
    p.set_defaults(func=cmd_guard)

    p = sub.add_parser(
        "build-all", help="build targets; silence is the pass condition")
    p.add_argument("--tree", required=True, help="the build tree")
    p.add_argument("--ok", required=True,
                   help="the line printed on a clean build")
    p.add_argument("targets", nargs=argparse.REMAINDER,
                   help="the targets (none: the whole tree)")
    p.set_defaults(func=cmd_build_all)

    p = sub.add_parser(
        "stale-trees", help="print '<dir>\\t<why>' for every stale build tree below ROOT")
    p.add_argument("root")
    p.set_defaults(func=cmd_stale_trees)

    p = sub.add_parser(
        "config", help="the tree suffix or the configure flags of a compiler/library pair")
    p.add_argument("--compiler", required=True, help="clang or gcc")
    p.add_argument("--libs", required=True, help="llvm or newlib")
    p.add_argument("what", choices=("suffix", "cmake-flags"))
    p.set_defaults(func=cmd_config)

    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
