"""The one runner of the SDK's must-fail suites: snippets that must not compile, each with the text the compiler must
print, and controls that must compile.

A suite is a HEAD, a list of `Case`s and `sys.exit(must_fail.run(HEAD, CASES))`. tests/CMakeLists.txt's
`kvasir_add_must_fail_test()` passes the command line:

    test_<x>.py --cxx <c++ compiler> --src <Kvasir_SDK/src> [--tests <Kvasir_SDK/tests>] [--flag=<flag>]...

The `--flag`s are the configuration's (the C++ standard, -stdlib=libc++, the standard library's debug mode), so the
snippets are checked against the standard library the tree builds with. Every case compiles `-fsyntax-only` in a file
of its own, up to four at once; the report keeps the order of the list.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import os
import subprocess
import tempfile
from dataclasses import dataclass, field
from pathlib import Path


@dataclass
class Case:
    """One snippet. `want` empty or None: a control, it must compile. A list: every substring must be in the output, in
    any order; a tuple: in this order. `clang_only` substrings are wanted only when the compiler is clang. `flags` are
    this case's extra compiler flags (`@TESTS@` and `@SRC@` are replaced by the directories). `head` replaces the
    suite's HEAD for this case."""

    name: str
    body: str
    want: list[str] | tuple[str, ...] | None = None
    clang_only: list[str] = field(default_factory=list)
    flags: list[str] = field(default_factory=list)
    head: str | None = None


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    p = argparse.ArgumentParser(description="compile must-fail snippets")
    p.add_argument("--cxx", required=True, help="the C++ compiler")
    p.add_argument("--src", required=True, help="Kvasir_SDK/src")
    p.add_argument(
        "--tests", help="Kvasir_SDK/tests (for suites that include its fixtures)")
    p.add_argument("--flag", action="append", default=[],
                   help="a compiler flag of the configuration (repeatable)")
    p.add_argument("--jobs", type=int, default=min(os.cpu_count()
                   or 1, 4), help="cases compiled at once")
    return p.parse_args(argv)


def _missing(want: list[str] | tuple[str, ...], out: str) -> list[str]:
    if isinstance(want, tuple):  # in this order: each after the end of the one before
        at, missing = 0, []
        for w in want:
            i = out.find(w, at)
            if i < 0:
                missing.append(w)
            else:
                at = i + len(w)
        return missing
    return [w for w in want if w not in out]


def _check(case: Case, rc: int, out: str, is_clang: bool) -> str | None:
    """None when the case holds, else the reason."""
    want = case.want
    if not want and not case.clang_only:
        return None if rc == 0 else f"the control does not compile (exit {rc})"
    wanted: list[str] | tuple[str, ...] = want or []
    if is_clang and case.clang_only:
        wanted = (tuple(wanted) + tuple(case.clang_only)
                  ) if isinstance(wanted, tuple) else [*wanted, *case.clang_only]
    missing = _missing(wanted, out)
    if rc == 0 or missing:
        return f"exit {rc}, missing {missing}"
    return None


def run(head: str, cases: list[Case], *, flags: list[str] | tuple[str, ...] = (), controls_first: list[Case] = (),
        argv: list[str] | None = None) -> int:
    """Compile every case and print `ok   <name>` or `FAIL <name>: <reason>` and 3000 characters of the compiler's
    output, in list order. `flags` are the suite's own (after the configuration's). A failing case of
    `controls_first` stops the run before the cases. Returns the exit code: 0 all held, 1 otherwise."""
    args = parse_args(argv)
    subst = {"@SRC@": args.src, "@TESTS@": args.tests or ""}
    base = [args.cxx, *args.flag, *flags, "-fsyntax-only", f"-I{args.src}"]
    if args.tests:
        base.append(f"-I{args.tests}")
    is_clang = False
    if any(c.clang_only for c in [*controls_first, *cases]):
        is_clang = "clang" in subprocess.run(
            [args.cxx, "--version"], capture_output=True, text=True).stdout

    def expand(s: str) -> str:
        for k, v in subst.items():
            s = s.replace(k, v)
        return s

    with tempfile.TemporaryDirectory() as tmp:

        def compile_case(index_case: tuple[int, Case]) -> tuple[int, str]:
            index, case = index_case
            f = Path(tmp) / f"{index}.cpp"
            f.write_text(
                (head if case.head is None else case.head) + case.body)
            r = subprocess.run(
                [*base, *[expand(x) for x in case.flags], str(f)], capture_output=True, text=True)
            return r.returncode, r.stdout + r.stderr

        def batch(group: list[Case], offset: int) -> int:
            failed = 0
            with concurrent.futures.ThreadPoolExecutor(max(1, args.jobs)) as pool:
                results = pool.map(compile_case, enumerate(group, offset))
                for case, (rc, out) in zip(group, results):
                    reason = _check(case, rc, out, is_clang)
                    if reason is None:
                        print(f"ok   {case.name}", flush=True)
                    else:
                        failed += 1
                        print(
                            f"FAIL {case.name}: {reason}\n{out[:3000]}", flush=True)
            return failed

        if controls_first and batch(list(controls_first), 0):
            return 1
        failed = batch(list(cases), len(controls_first))
    print(f"{len(cases) + len(controls_first) - failed} ok, {failed} failed")
    return 1 if failed else 0
