#!/usr/bin/env python3
"""What a translation unit's headers cost: preprocessed lines, files read, and where the lines come from.

The measure is the TU's own compile command from a configured tree's `compile_commands.json`, rerun with `-E`
(nothing is built, nothing in the tree is written):

    header_cost.py measure <tree> <target>[:<file suffix>]... [--top N] [--instructions] [--repeat N] [--json]
    header_cost.py check <ceilings.toml> [--tree <dir>] [--instructions]

`measure` prints, per TU: the non-empty lines of the preprocessed text (the `-E -P` count), the number of files read,
the share of generated headers (paths containing `--generated` text, default `svd_converter_generated`), the lines
per include root (the `-I`/`-isystem` directory a file was found under) and the `--top` heaviest files.
`<target>` is the CMake target whose object directory (`<target>.dir/`) the command writes into; the file suffix
(default `main.cpp`) picks the source when the target has several.

`--instructions` adds `perf stat -e instructions:u` of the same command with `-fsyntax-only` (`--repeat` runs, the
mean). It depends on the machine and the compiler build, so it is opt-in and never a ceiling.

`check` reads a ceilings file:

    margin_percent = 2            # a TU may grow this much over its ceiling before the check fails
    [[tu]]
    tree = "build"                # relative to the ceilings file
    target = "release"
    file = "src/main.cpp"         # optional suffix, default main.cpp
    lines = 123456                # the ceiling: non-empty preprocessed lines

and exits 1 when a TU's lines exceed `lines * (1 + margin_percent / 100)`, 2 when a tree, a target or a compile
fails. A TU far below its ceiling is reported (lower the ceiling), not failed. `--tree <dir>` measures every `[[tu]]`
in that tree instead of its own `tree` (a CI job that builds into another directory than the file names).
"""

import argparse
import collections
import json
import os
import re
import shlex
import subprocess
import sys
import tomllib

DROP_WITH_VALUE = {"-o", "-MF", "-MT", "-MQ"}
DROP = {"-c", "-MD", "-MMD", "-fwhole-program-vtables"}
MARKER = re.compile(r'^# (\d+) "((?:[^"\\]|\\.)*)"')


class CostError(Exception):
    """A tree, target or compile that cannot be measured (exit 2)."""


def load_entry(tree, target, suffix):
    path = os.path.join(tree, "compile_commands.json")
    try:
        with open(path, encoding="utf-8") as f:
            entries = json.load(f)
    except OSError as e:
        raise CostError(f"{path}: {e.strerror}") from e
    needle = f"/{target}.dir/"
    hits = [e for e in entries if e["file"].endswith(suffix)
            and needle in (e.get("output", "") + " " + e.get("command", " ".join(e.get("arguments", []))))]
    if not hits:
        raise CostError(
            f"{tree}: no compile command of target '{target}' for a file ending in '{suffix}'")
    if len({e["file"] for e in hits}) > 1:
        names = ", ".join(sorted({e["file"] for e in hits}))
        raise CostError(
            f"{tree}: '{target}:{suffix}' names several files ({names}): give a longer suffix")
    return hits[0]


def base_args(entry):
    """The entry's command without its outputs, dependency files and LTO switches."""
    args = entry["arguments"] if "arguments" in entry else shlex.split(
        entry["command"])
    out, skip = [], False
    for a in args:
        if skip:
            skip = False
            continue
        if a in DROP_WITH_VALUE:
            skip = True
            continue
        if a in DROP or a.startswith("-flto"):
            continue
        out.append(a)
    return out


def include_roots(args, directory):
    roots, take = [], False
    for a in args:
        if take:
            roots.append(os.path.normpath(os.path.join(directory, a)))
            take = False
            continue
        flag = next((f for f in ("-isystem", "-iquote",
                    "-idirafter", "-I") if a.startswith(f)), None)
        if flag is None:
            continue
        if a == flag:
            take = True
        else:
            roots.append(os.path.normpath(
                os.path.join(directory, a[len(flag):])))
    return sorted(set(roots), key=len, reverse=True)


def unescape(name):
    return re.sub(r"\\(.)", r"\1", name)


def preprocess(entry):
    """Lines per file from `-E` with line markers; the non-marker non-empty lines are what `-E -P` keeps."""
    args = base_args(entry)
    run = subprocess.run(
        args + ["-E", "-o", "-"], cwd=entry["directory"], capture_output=True, text=True)
    if run.returncode:
        raise CostError(
            f"{entry['file']}: preprocessing failed:\n{run.stderr[-3000:]}")
    per_file = collections.Counter()
    seen = set()
    # files entered from "<command-line>": gcc's implicit stdc-predef.h, -include files
    preincluded = set()
    current = entry["file"]
    for line in run.stdout.splitlines():
        m = MARKER.match(line)
        if m:
            name = unescape(m.group(2))
            if current.startswith("<") and not name.startswith("<") and name != entry["file"]:
                preincluded.add(name)
            current = name
            seen.add(current)
            continue
        if line.strip():
            per_file[current] += 1
    # a pre-include that adds no line (stdc-predef.h is macros only) is not a file this source pulls in
    files = {f for f in seen if not f.startswith("<") and not (f in preincluded and per_file[f] == 0)}
    return per_file, files, args


def instructions(entry, args, repeat):
    cmd = ["perf", "stat", "-x", ",", "-e", "instructions:u",
           "-r", str(repeat), "--"] + args + ["-fsyntax-only"]
    run = subprocess.run(
        cmd, cwd=entry["directory"], capture_output=True, text=True)
    if run.returncode:
        raise CostError(
            f"{entry['file']}: perf stat failed:\n{run.stderr[-2000:]}")
    for line in run.stderr.splitlines():
        parts = line.split(",")
        if len(parts) > 2 and parts[2].startswith("instructions"):
            return int(float(parts[0]))
    raise CostError(
        f"perf printed no instructions count:\n{run.stderr[-2000:]}")


def measure(tree, target, suffix, generated, want_instructions=False, repeat=3):
    entry = load_entry(tree, target, suffix)
    per_file, files, args = preprocess(entry)
    roots = include_roots(args, entry["directory"])
    by_root = collections.Counter()
    for f, n in per_file.items():
        path = os.path.normpath(os.path.join(entry["directory"], f))
        root = next((r for r in roots if path.startswith(
            r + os.sep)), "(source and other)")
        by_root[root] += n
    total = sum(per_file.values())
    gen_lines = sum(n for f, n in per_file.items() if generated in f)
    result = {
        "tree": tree, "target": target, "file": entry["file"], "lines": total,
        "files": len(files), "generated_files": sum(1 for f in files if generated in f),
        "generated_lines": gen_lines, "by_root": dict(by_root.most_common()),
        "by_file": dict(per_file.most_common()),
    }
    if want_instructions:
        result["instructions"] = instructions(entry, args, repeat)
    return result


def shown(path):
    rel = os.path.relpath(path)
    return path if rel.startswith("..") else rel


def share(part, whole):
    return f"{100.0 * part / whole:.1f} %" if whole else "-"


def print_result(r, top):
    print(f"{r['tree']} {r['target']} {os.path.basename(r['file'])}: {r['lines']} lines, {r['files']} files, "
          f"generated {r['generated_files']} files / {r['generated_lines']} lines ({share(r['generated_lines'], r['lines'])})"
          + (f", {r['instructions'] / 1e9:.3f} G instructions" if "instructions" in r else ""))
    for root, n in list(r["by_root"].items())[:top]:
        print(f"  {n:>9}  {share(n, r['lines']):>7}  {shown(root)}")
    if top:
        print("  heaviest files:")
        for f, n in list(r["by_file"].items())[:top]:
            print(f"  {n:>9}  {share(n, r['lines']):>7}  {shown(f)}")


def split_spec(spec):
    target, _, suffix = spec.partition(":")
    return target, suffix or "main.cpp"


def cmd_measure(a):
    results = []
    for spec in a.tus:
        target, suffix = split_spec(spec)
        r = measure(a.tree, target, suffix, a.generated,
                    a.instructions, a.repeat)
        results.append(r)
        if not a.json:
            print_result(r, a.top)
    if a.json:
        for r in results:
            r.pop("by_file")
        json.dump(results, sys.stdout, indent=2)
        print()
    return 0


def cmd_check(a):
    try:
        with open(a.ceilings, "rb") as f:
            cfg = tomllib.load(f)
    except (OSError, tomllib.TOMLDecodeError) as e:
        raise CostError(f"{a.ceilings}: {e}") from e
    unknown = set(cfg) - {"margin_percent", "generated", "tu"}
    if unknown or not cfg.get("tu"):
        raise CostError(
            f"{a.ceilings}: needs [[tu]] tables (unknown keys: {', '.join(sorted(unknown)) or 'none'})")
    margin = float(cfg.get("margin_percent", 2))
    generated = cfg.get("generated", a.generated)
    base = os.path.dirname(os.path.abspath(a.ceilings))
    failed = 0
    for tu in cfg["tu"]:
        keys = set(tu) - {"tree", "target", "file", "lines"}
        if keys or not {"tree", "target", "lines"} <= set(tu):
            raise CostError(
                f"{a.ceilings}: a [[tu]] needs tree, target, lines (unknown: {', '.join(sorted(keys))})")
        tree = os.path.abspath(a.tree) if a.tree else os.path.normpath(
            os.path.join(base, tu["tree"]))
        r = measure(tree, tu["target"], tu.get(
            "file", "main.cpp"), generated, a.instructions, a.repeat)
        limit = tu["lines"] * (1 + margin / 100)
        name = f"{a.tree or tu['tree']} {tu['target']} {tu.get('file', 'main.cpp')}"
        extra = f", {r['instructions'] / 1e9:.3f} G instructions" if "instructions" in r else ""
        if r["lines"] > limit:
            failed += 1
            print(
                f"OVER     {name}: {r['lines']} lines > ceiling {tu['lines']} + {margin:g} %{extra}")
            for root, n in list(r["by_root"].items())[:5]:
                print(f"           {n:>9}  {shown(root)}")
        elif r["lines"] < tu["lines"] * (1 - 2 * margin / 100):
            print(
                f"BELOW    {name}: {r['lines']} lines, ceiling {tu['lines']} (lower it){extra}")
        else:
            print(
                f"ok       {name}: {r['lines']} lines, ceiling {tu['lines']} + {margin:g} %{extra}")
    return 1 if failed else 0


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    # after the subcommand, as the usage above writes them (a top-level option would be refused there)
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--generated", default="svd_converter_generated",
                        help="path text that marks a generated header (default: svd_converter_generated)")
    common.add_argument("--instructions", action="store_true",
                        help="also perf-stat -fsyntax-only (machine-dependent)")
    common.add_argument("--repeat", type=int, default=3,
                        help="perf stat runs (default 3)")
    sub = p.add_subparsers(dest="cmd", required=True)
    m = sub.add_parser("measure", parents=[
                       common], help="print the cost of TUs of one tree")
    m.add_argument("tree")
    m.add_argument("tus", nargs="+", metavar="target[:file-suffix]")
    m.add_argument("--top", type=int, default=8,
                   help="include roots and files listed per TU (default 8)")
    m.add_argument("--json", action="store_true")
    c = sub.add_parser("check", parents=[
                       common], help="fail when a TU exceeds its ceiling")
    c.add_argument("ceilings")
    c.add_argument(
        "--tree", help="measure every [[tu]] in this tree (relative to the working directory)")
    a = p.parse_args(argv)
    try:
        return cmd_measure(a) if a.cmd == "measure" else cmd_check(a)
    except CostError as e:
        print(f"header_cost: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
