#!/usr/bin/env python3
"""kvasir_test: run a Kvasir::Test image (Kvasir_SDK/src/kvasir/Test) on its board through the log printer, judge it.

    kvasir_test.py list BUILD TARGET
    kvasir_test.py run  BUILD TARGET [--no-flash] [--no-build] [--filter GLOB]... [--exclude GLOB]... [--board NAME]
                        [--stop-at-first-failure] [--repeat N] [--timeout S] [--junit FILE] [--json FILE]
    kvasir_test.py run  BUILD TARGET --done REGEX [--pass REGEX] ...      (a self-judging image: its DONE line)

The list comes from the image's symbols, never from a boot: every listed test is a function
`Kvasir::Test::Detail::Slot<I, KvasirTests::<suite>::<name>>::run`, every defined one has
`KvasirTests::<suite>::<name>::body` (kept in the image whether listed or not) - a body without a slot is a test that
was forgotten in the list (`list` exits 1).

`run`: the printer of the target (a running one - `just log` - is used and left running; else one is started and
stopped again at the end, as kvasir_bench does); the image built and flashed through it (unless --no-flash); the
image's TEST|READY (or TEST|DONE) line awaited; the request written to `kvasir_test_request` (magic cleared, words 1..,
the magic again); TEST|START|seq=<seq> awaited (none within the image's host wait + 2 s: one reset through the printer,
the next boot takes the request). Every READY line carries the image's build id - the first 8 bytes of its GNU build-id
note, the bytes the log printer's firmware check compares - and its host wait: a READY from another image than the
ELF's (`_LINKER_build_id_`) is a stale board, refused (exit 4) without a flash compare, with or without --no-flash; the TEST| lines followed to TEST|DONE|...|seq=<seq> (a DONE with another seq is stale); then the run
state `kvasir_test_run` read and decoded - its verdicts win over the log, which can lose lines; a sanitize image's UB
counter read. A test without its log line is reported "lines lost".

Exit codes (as kvasir_bench.py): 0 every selected test passed or was skipped, 1 a test failed (or an argument / the
build / the list was wrong), 2 timeout or an incomplete run, 3 the board is in trouble (the core halted, sanitizer
reports), 4 refused by a guard, 77 the tree's probe is not on USB (ctest: skipped).

`<repo>/.kvasir_test.json` (the repo above the build tree): {"boards": {"boardA": "build", "boardB": "build_b"},
"skip": {"boardB": ["perf.*"]}} - --board NAME takes BUILD as the repo and the tree from "boards"; "skip" excludes
tests (globs on "suite.name") on that board.
"""
import argparse
import dataclasses
import fnmatch
import json
import re
import socket
import subprocess
import sys
import time
import typing
import xml.etree.ElementTree as ET
import zlib
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))

import kvasir_bench_lib  # noqa: E402,F401  (puts uc_log's tools on sys.path)
from kvasir_bench_lib import printer, trees  # noqa: E402
from kvasir_bench_lib.session import Session  # noqa: E402
from uc_log_client import (ControlError, Connection, message_kind, req_flash, req_reset,  # noqa: E402
                           req_status, req_subscribe, request)

EXIT_OK, EXIT_FAIL, EXIT_TIMEOUT, EXIT_TROUBLE, EXIT_REFUSED, EXIT_NO_BOARD = 0, 1, 2, 3, 4, 77

# "KTRQ" in memory (RunState.hpp Request::magicValue)
REQUEST_MAGIC = 0x5152_544B
RUN_ID = 0x5453_544B          # "KTST" (RunState.hpp RunTag)
MAX_TESTS = 256
STOP_AT_FIRST_FAILURE = 1     # Request::stopAtFirstFailure
STATE_WORDS = 29              # sizeof(RunState) / 4
HOST_WAIT_S = 3.0             # DefaultConfig::hostWait
VERDICTS = {0: "none", 1: "pass", 2: "fail", 3: "skip"}


# ---- the image's list ---------------------------------------------------------------------------------------------

@dataclasses.dataclass(frozen=True)
class TestId:
    index: int
    suite: str
    name: str

    @property
    def label(self) -> str:
        return f"{self.suite}.{self.name}"


def _source_names(mangled: str, pos: int) -> tuple[list[str], int]:
    """The length-prefixed identifiers of an Itanium nested name from `pos` (after its `N`) to its `E`."""
    names = []
    while pos < len(mangled) and mangled[pos].isdigit():
        m = re.match(r"\d+", mangled[pos:])
        n = int(m.group(0))
        pos += len(m.group(0))
        names.append(mangled[pos:pos + n])
        pos += n
    return names, pos


def _suite_and_name(names: list[str]) -> tuple[str, str] | None:
    """The two components after KvasirTests (the macros' namespace; a user namespace may come before it)."""
    if "KvasirTests" not in names:
        return None
    i = names.index("KvasirTests")
    return (names[i + 1], names[i + 2]) if i + 2 < len(names) else None


SLOT = re.compile(r"_ZN6Kvasir4Test6Detail4SlotILj(\d+)EN")


def tests_in(mangled_nm: str) -> tuple[list[TestId], set[tuple[str, str]]]:
    """(listed tests by index, defined (suite, name)s) from `llvm-nm --no-demangle` text."""
    listed: dict[int, TestId] = {}
    defined: set[tuple[str, str]] = set()
    for line in mangled_nm.splitlines():
        sym = line.split()[-1] if line.split() else ""
        m = SLOT.match(sym)
        if m:
            names, _ = _source_names(sym, m.end())
            sn = _suite_and_name(names)
            if sn:
                listed[int(m.group(1))] = TestId(int(m.group(1)), *sn)
            continue
        if sym.startswith("_ZN") and "11KvasirTests" in sym:
            names, end = _source_names(sym, 3)
            if names[-1:] == ["body"] and sym[end:end + 1] == "E":
                sn = _suite_and_name(names[:-1])
                if sn:
                    defined.add(sn)
    return [listed[i] for i in sorted(listed)], defined


def unlisted(listed: list[TestId], defined: set[tuple[str, str]]) -> list[tuple[str, str]]:
    have = {(t.suite, t.name) for t in listed}
    return sorted(defined - have)


def symbol_address(mangled_nm: str, name: str) -> int | None:
    for line in mangled_nm.splitlines():
        parts = line.split()
        if parts and parts[-1] == name:
            return int(parts[0], 16)
    return None


# ---- selection, request, run state ---------------------------------------------------------------------------------

def select(tests: list[TestId], filters: list[str], excludes: list[str]) -> set[int]:
    chosen = set()
    for t in tests:
        if filters and not any(fnmatch.fnmatchcase(t.label, f) for f in filters):
            continue
        if any(fnmatch.fnmatchcase(t.label, e) for e in excludes):
            continue
        chosen.add(t.index)
    return chosen


def request_words(seq: int, flags: int, selected: set[int]) -> list[int]:
    """The request block (RunState.hpp Request): magic, seq, flags, select[8] - 11 words."""
    sel = [0] * (MAX_TESTS // 32)
    for i in selected:
        sel[i // 32] |= 1 << (i % 32)
    return [REQUEST_MAGIC, seq, flags, *sel]


def request_writes(address: int, words: list[int]) -> list[tuple[int, int]]:
    """(address, value) in the order the runner needs, for ONE write request: the magic cleared first (the last
    request's magic is still valid in the block - a new seq written under it would be taken with the old selection),
    then words 1.., then the magic again."""
    return [(address, 0)] + [(address + 4 * i, w) for i, w in enumerate(words) if i != 0] + [(address, words[0])]


@dataclasses.dataclass
class RunState:
    request_seq: int
    next: int
    in_flight: int
    passed: int
    failed: int
    skipped: int
    boots: int
    phase: int
    flags: int
    count: int
    verdicts: list[str]
    selected: set[int]

    def served_seq(self) -> int:
        return self.request_seq if self.flags & 1 else 0


def decode_run_state(data: bytes) -> RunState | None:
    """kvasir_test_run (Persistent<RunState, RunTag>): id, 29 words, CRC-32 (zlib) over the words before it. None when
    the id or the CRC does not match (never stored, torn, garbage after a power-up)."""
    if len(data) < 4 * (STATE_WORDS + 2):
        return None
    w = [int.from_bytes(data[4 * i:4 * i + 4], "little")
         for i in range(STATE_WORDS + 2)]
    if w[0] != RUN_ID or w[-1] != zlib.crc32(data[:4 * (STATE_WORDS + 1)]):
        return None
    v = w[1:1 + STATE_WORDS]

    def half(word: int, hi: bool) -> int:
        return (word >> 16) & 0xFFFF if hi else word & 0xFFFF
    count = half(v[4], True)
    verdict_words, select_words = v[5:21], v[21:29]
    verdicts = [VERDICTS[(verdict_words[i // 16] >> (2 * (i % 16))) & 3]
                for i in range(count)]
    selected = {i for i in range(count) if (
        select_words[i // 32] >> (i % 32)) & 1}
    return RunState(request_seq=v[0], next=half(v[1], False), in_flight=half(v[1], True),
                    passed=half(v[2], False), failed=half(v[2], True), skipped=half(v[3], False),
                    boots=half(v[3], True), phase=v[4] & 0xFF, flags=(v[4] >> 8) & 0xFF, count=count,
                    verdicts=verdicts, selected=selected)


# ---- the image's build id ------------------------------------------------------------------------------------------

# uc_log detail/BuildId.hpp buildIdNoteSymbol (the SDK's linker scripts)
BUILD_ID_NOTE = "_LINKER_build_id_"


def build_id_of(note: bytes | None) -> str | None:
    """The build id a Kvasir::Test image says in its READY line, from the 24 bytes at its GNU build-id note: name size
    4, hash size >= 8, type 3 (NT_GNU_BUILD_ID), "GNU\\0", then the hash, whose first 8 bytes are two little-endian
    words printed as 16 hex digits - the bytes uc_log's firmware check compares (detail/BuildId.hpp). None: no note."""
    if note is None or len(note) < 24:
        return None
    w = [int.from_bytes(note[i:i + 4], "little") for i in range(0, 24, 4)]
    if w[0] != 4 or w[1] < 8 or w[2] != 3 or w[3] != 0x0055_4E47:
        return None
    return f"{w[4]:08x}{w[5]:08x}"


def elf_build_id(elf) -> str | None:
    """build_id_of the note in an ELF (kvasir_elf.Elf)."""
    sym = elf.symbol(BUILD_ID_NOTE)
    return build_id_of(elf.bytes_at(sym[0], 24)) if sym else None


def image_build_id(tree) -> str | None:
    """elf_build_id of a tree's image (kvasir_bench_lib Tree: `image.parsed` is the parsed ELF, a property)."""
    return elf_build_id(tree.image.parsed)


def check_image(res: "Results", build_id: str | None) -> None:
    """A READY line from another image than the ELF's: the board runs a stale image (refused, exit 4)."""
    if build_id and res.ready_build and res.ready_build != build_id:
        fail(f"the board runs another image: its READY says build {res.ready_build}, this ELF's note is {build_id} "
             "(flash it)", EXIT_REFUSED)


# ---- the lines -----------------------------------------------------------------------------------------------------

LINE = {
    "ready": re.compile(r"TEST\|READY\|n=(\d+)\|seq=(\d+)\|build=([0-9a-f]{16})\|wait=(\d+)ms"),
    "start": re.compile(r"TEST\|START\|seq=(\d+)\|n=(\d+)\|of=(\d+)"),
    "done": re.compile(r"TEST\|DONE\|pass=(\d+)\|fail=(\d+)\|skip=(\d+)\|boots=(\d+)\|seq=(\d+)"),
    "pass": re.compile(r"TEST\|PASS\|(\d+)\|[^|]*\|(\d+)us"),
    "fail": re.compile(r"TEST\|FAIL\|(\d+)\|[^|]*\|(.*)"),
    "skipped": re.compile(r"TEST\|SKIPPED\|(\d+)\|(.*)"),
    "skip": re.compile(r"TEST\|SKIP\|(\d+)\|[^|]*(?:\|(.*))?"),
    "expect": re.compile(r"TEST\|EXPECT\|(\d+)\|(.*)"),
    "reset": re.compile(r"TEST\|RESET\|(\d+)\|[^|]*\|(.*)"),
    "after": re.compile(r"TEST\|AFTER\|(\d+)\|[^|]*\|([\w:]+)\|([\w:]+)\|?(.*)"),
    "perf": re.compile(r"TEST\|PERF\|(\d+)\|[^|]*\|n=(\d+)\|min=(\d+)\|mean=(\d+)\|max=(\d+)\|(\w+)"),
    "note": re.compile(r"TEST\|NOTE\|(\d+)\|(.*)"),
}


def _verdict_word(text: str) -> str:
    """An enum as the printer shows it: `pass`, `Verdict::pass` or its number."""
    word = text.split("::")[-1].lower()
    return VERDICTS.get(int(word), word) if word.isdigit() else word


@dataclasses.dataclass
class TestResult:
    test: TestId
    selected: bool = True
    # from the log; the state's replaces it (reconcile)
    verdict: str = "none"
    time_us: int | None = None
    perf: dict | None = None
    after: str | None = None
    detail: str = ""
    expects: list[str] = dataclasses.field(default_factory=list)
    lines: list[str] = dataclasses.field(default_factory=list)
    lines_lost: bool = False


class Results:
    """The TEST| lines of one run, by index, and its markers."""

    def __init__(self, tests: list[TestId], selected: set[int]):
        self.tests = {t.index: TestResult(
            t, t.index in selected) for t in tests}
        self.ready: int | None = None
        # the READY line's build id (16 hex digits), when it has one
        self.ready_build: str | None = None
        self.ready_wait: float | None = None   # its host wait, s
        self.started: int | None = None   # the seq of the START line seen
        self.done: dict | None = None
        self.boots = 0
        self.notes: list[str] = []

    def feed(self, text: str, seq: int | None = None) -> str | None:
        """One log message; returns "ready", "start", "done" for the markers (a DONE of another seq: "stale")."""
        if "TEST|" not in text:
            if re.search(r"\bboot: ", text) or text.startswith("boot:"):
                self.boots += 1
            return None
        for kind, rx in LINE.items():
            m = rx.search(text)
            if not m:
                continue
            if kind == "ready":
                self.ready = int(m.group(1))
                self.ready_build, self.ready_wait = m.group(
                    3), int(m.group(4)) / 1000
                return "ready"
            if kind == "start":
                self.started = int(m.group(1))
                return "start"
            if kind == "done":
                done = dict(
                    zip(("pass", "fail", "skip", "boots", "seq"), map(int, m.groups())))
                if seq is not None and done["seq"] != seq:
                    return "stale"
                self.done = done
                return "done"
            r = self.tests.get(int(m.group(1)))
            if r is None:
                self.notes.append(
                    f"a line for test {m.group(1)}, which the image does not list: {text}")
                return None
            r.lines.append(text[text.find("TEST|"):])
            if kind == "pass":
                r.verdict, r.time_us = "pass", int(m.group(2))
            elif kind == "fail":
                r.verdict, r.detail = "fail", m.group(2)
                t = re.search(r"\|(\d+)us$", text)
                r.time_us = int(t.group(1)) if t else r.time_us
            elif kind == "skip":
                r.verdict = "skip"
                r.detail = r.detail or (m.group(2) or "")
            elif kind == "skipped":
                r.detail = m.group(2)
            elif kind == "expect":
                r.expects.append(m.group(2))
            elif kind == "after":
                r.verdict = _verdict_word(m.group(2))
                r.after = f"{m.group(3).split('::')[-1]} {m.group(4)}".strip()
            elif kind == "perf":
                r.perf = dict(zip(("n", "min", "mean", "max"), map(
                    int, m.groups()[1:5]))) | {"unit": m.group(6)}
            elif kind == "note":
                r.detail = m.group(2)
            return None
        return None

    def clear_tests(self) -> None:
        for i, r in self.tests.items():
            self.tests[i] = TestResult(r.test, r.selected)

    def reconcile(self, state: RunState | None) -> None:
        """The state's verdicts win; a verdict the log did not carry is "lines lost"."""
        if state is None:
            return
        for i, r in self.tests.items():
            if i >= len(state.verdicts):
                continue
            v = state.verdicts[i]
            if v != "none" and r.verdict != v:
                r.lines_lost = r.verdict == "none"
                r.verdict = v

    def selected(self) -> list[TestResult]:
        return [r for _, r in sorted(self.tests.items()) if r.selected]

    def counts(self) -> dict[str, int]:
        c = {"pass": 0, "fail": 0, "skip": 0, "none": 0}
        for r in self.selected():
            c[r.verdict if r.verdict in c else "fail"] += 1
        return c


def text_of(event: dict) -> str:
    return event.get("message", "")


def junit(runs: list[Results], target: str) -> ET.ElementTree:
    root = ET.Element("testsuites", name=target)
    for k, res in enumerate(runs):
        suites: dict[str, ET.Element] = {}
        for r in res.selected():
            name = r.test.suite + (f"#{k + 1}" if len(runs) > 1 else "")
            suite = suites.get(name)
            if suite is None:
                suite = suites[name] = ET.SubElement(
                    root, "testsuite", name=name)
            case = ET.SubElement(suite, "testcase", classname=r.test.suite, name=r.test.name,
                                 time=f"{(r.time_us or 0) / 1e6:.6f}")
            if r.verdict == "fail":
                f = ET.SubElement(
                    case, "failure", message=r.detail or r.after or "failed")
                f.text = "\n".join(r.expects)
            elif r.verdict == "skip":
                ET.SubElement(case, "skipped", message=r.detail)
            elif r.verdict == "none":
                ET.SubElement(
                    case, "error", message="did not run (incomplete run)")
            out = ET.SubElement(case, "system-out")
            out.text = "\n".join(
                r.lines) + ("\n(lines lost: the verdict is the run state's)" if r.lines_lost else "")
        for suite in suites.values():
            cases = suite.findall("testcase")
            suite.set("tests", str(len(cases)))
            suite.set("failures", str(
                sum(1 for c in cases if c.find("failure") is not None)))
            suite.set("errors", str(
                sum(1 for c in cases if c.find("error") is not None)))
            suite.set("skipped", str(
                sum(1 for c in cases if c.find("skipped") is not None)))
    return ET.ElementTree(root)


def as_json(runs: list[Results]) -> list[dict]:
    return [{"done": res.done,
             "tests": [{"index": r.test.index, "suite": r.test.suite, "name": r.test.name, "selected": r.selected,
                        "verdict": r.verdict, "time_us": r.time_us, "perf": r.perf, "after": r.after,
                        "detail": r.detail, "expects": r.expects, "lines_lost": r.lines_lost}
                       for _, r in sorted(res.tests.items())]} for res in runs]


def table(res: Results) -> list[str]:
    out = []
    for r in res.selected():
        if r.perf:
            what = (f"mean {r.perf['mean']} {r.perf['unit']} (min {r.perf['min']}, max {r.perf['max']}, "
                    f"n={r.perf['n']})")
        elif r.after:
            what = f"(after reset: {r.after})"
        elif r.time_us is not None:
            what = f"{r.time_us}us"
        else:
            what = ""
        if r.detail and r.verdict in ("fail", "skip"):
            what = (what + "  " + r.detail).strip()
        if r.lines_lost:
            what += "  (lines lost)"
        out.append(
            f"  {r.test.index:3} {r.test.label:40} {r.verdict.upper():5} {what}")
        out += [f"        {e}" for e in r.expects]
    return out


# ---- a board that is away for a moment ------------------------------------------------------------------------------

class BoardBusy(Exception):
    """The target is away for a moment (an announced reset: the probe stays off it) - try again."""


# longer than any stay-away a test image asks for (a watchdog test's timeout + 1.5 s)
BUSY_FOR_S = 15.0


def patient(op: typing.Callable[[], typing.Any], board: "Board", now: typing.Callable[[], float],
            seconds: float = BUSY_FOR_S):
    """op(), again while the board is busy (BoardBusy), up to `seconds`: a run the image started without the host (its
    host wait ran out) resets through its cross-reset tests while the host reads the run state or writes its request."""
    end = now() + seconds
    while True:
        try:
            return op()
        except BoardBusy as e:
            if now() >= end:
                fail(
                    f"the board stayed away for {seconds:g} s: {e}", EXIT_TROUBLE)
            board.pause()


# ---- the board, through the printer ---------------------------------------------------------------------------------

class Board(typing.Protocol):
    def build(self) -> None: ...
    def flash(
        self) -> int: ...                     # returns the log seq the run's events start at

    def mark_now(self) -> int: ...
    def reset(self) -> None: ...
    def events(
        self, since: int) -> typing.Iterator[dict | None]: ...   # None: a quiet tick

    def read(self, address: int, size: int) -> bytes: ...
    def write(self, writes: list[tuple[int, int]]) -> None: ...
    def ub_count(self) -> int | None: ...
    # the printer's firmware check: match, different, unchecked, ...
    def firmware_state(self) -> str: ...
    def pause(self) -> None: ...                    # a moment's wait before an access that met BoardBusy is retried


class PrinterBoard:
    """A Board through the target's printer (kvasir_bench_lib: started here when none runs, stopped at exit)."""

    def __init__(self, tree: trees.Tree):
        self.tree = tree
        self.session = Session(tree, "kvasir_test")

    @property
    def control(self) -> Path:
        return self.session.control

    def build(self) -> None:
        built = subprocess.run(["cmake", "--build", str(self.tree.build), "--target", self.tree.target],
                               capture_output=True, text=True)
        if built.returncode != 0:
            bad = [l for l in (built.stdout + built.stderr).splitlines()
                   if re.search(r"error|FAILED|warning", l)]
            fail("the build failed:\n" +
                 "\n".join((bad or built.stdout.splitlines())[-15:]))

    def flash(self) -> int:
        printer.need_ram_image_printer(self.tree, self.control, "flash")
        printer.set_mark(self.tree, self.control)
        try:
            request(self.control, req_flash(), timeout=150)
        except ControlError as e:
            fail(f"flash through the printer: {e}")
        print(printer.firmware_line(self.tree, self.control))
        return printer.mark_seq(self.tree, self.control)

    def mark_now(self) -> int:
        printer.set_mark(self.tree, self.control)
        return printer.mark_seq(self.tree, self.control)

    def firmware_state(self) -> str:
        return str(request(self.control, req_status())["firmware"]["state"])

    def reset(self) -> None:
        try:
            request(self.control, req_reset(), timeout=40)
        except ControlError as e:
            fail(f"reset through the printer: {e}")

    def events(self, since: int) -> typing.Iterator[dict | None]:
        with Connection(self.control, 10) as stream:
            stream.ask(req_subscribe(["log", "messages"], since_seq=since))
            while True:
                stream.sock.settimeout(0.5)
                try:
                    yield stream.next()
                except (TimeoutError, socket.timeout):
                    yield None

    def read(self, address: int, size: int) -> bytes:
        data = printer.read_chunked(
            self.session.ask, address, size, on_error="none")
        if data is None:
            raise BoardBusy(f"read of {size} B at {address:#x} refused")
        return data

    def write(self, writes: list[tuple[int, int]]) -> None:
        try:
            self.session.write(writes)
        except ControlError as e:
            raise BoardBusy(str(e)) from e

    def pause(self) -> None:
        time.sleep(0.25)

    def ub_count(self) -> int | None:
        counter = self.session.records(["ubsan"]).get("ubsan")
        if counter is None or not counter.get("valid"):
            return None
        return counter["record"]["count"]


class Outcome(Exception):
    def __init__(self, code: int, message: str):
        super().__init__(message)
        self.code = code


def fail(message: str, code: int = EXIT_FAIL) -> typing.NoReturn:
    raise Outcome(code, message)


# ---- the run --------------------------------------------------------------------------------------------------------

@dataclasses.dataclass
class Options:
    flash: bool = True
    build: bool = True
    filters: list[str] = dataclasses.field(default_factory=list)
    excludes: list[str] = dataclasses.field(default_factory=list)
    stop_at_first_failure: bool = False
    repeat: int = 1
    timeout: float = 120.0
    sanitize: bool = False
    # the ELF's (build_id_of): a READY line with another one is a stale image
    build_id: str | None = None


class Follower:
    """The run's one event stream, with the deadline on the injected clock."""

    def __init__(self, board: Board, since: int, now: typing.Callable[[], float]):
        self.stream = board.events(since)
        self.now = now

    def until(self, deadline: float) -> typing.Iterator[dict]:
        """Log events until the deadline; a core halt ends the run (exit 3)."""
        while self.now() < deadline:
            event = next(self.stream, StopIteration)
            if event is StopIteration:
                return
            if event is None:
                continue
            if event.get("cmd") == "message":
                if message_kind(event) == "halt":
                    fail(
                        f"the core halted: {event.get('text', '')} (kvasir_bench.py crash)", EXIT_TROUBLE)
                continue
            if event.get("cmd") == "log":
                yield event


def run_once(board: Board, tests: list[TestId], selected: set[int], nm: str, opts: Options,
             follower: Follower, now: typing.Callable[[], float], wait_ready: bool) -> Results:
    req_at = symbol_address(nm, "kvasir_test_request")
    run_at = symbol_address(nm, "kvasir_test_run")
    if req_at is None or run_at is None:
        fail("the image has no kvasir_test_request / kvasir_test_run: not a Kvasir::Test image (try --done)")
    res = Results(tests, selected)

    if wait_ready:
        # the image is up and ours: READY (it waits for us) or DONE (it ran already, now idle)
        deadline = now() + 30
        for e in follower.until(deadline):
            mark = res.feed(text_of(e))
            if mark == "ready":
                check_image(res, opts.build_id)
            if mark == "ready" and res.ready != len(tests):
                fail(f"the image says it lists {res.ready} tests, its ELF lists {len(tests)}: does the board run "
                     "this build?")
            if mark in ("ready", "done"):
                break
        else:
            fail(
                "no TEST|READY or TEST|DONE line within 30 s after the flash", EXIT_TIMEOUT)
        host_wait = res.ready_wait
        res = Results(tests, selected)
    else:
        host_wait = None
    # the image's own host wait from its READY line; an image whose READY was before the mark: the default
    host_wait = HOST_WAIT_S if host_wait is None else host_wait

    # the printer's firmware check (the RAM copy of the build id, else the flash): a board that runs another image is
    # refused before a request goes to it - the printer does not decode that image's log, so its READY line (the check
    # below) never comes
    if board.firmware_state() == "different":
        fail("the printer says the board runs another image than this tree's (flash it)", EXIT_REFUSED)
    state = decode_run_state(patient(lambda: board.read(
        run_at, 4 * (STATE_WORDS + 2)), board, now))
    current = int.from_bytes(
        patient(lambda: board.read(req_at + 4, 4), board, now), "little")
    seq = (max(state.request_seq if state else 0, current) + 1) & 0xFFFF_FFFF or 1
    flags = STOP_AT_FIRST_FAILURE if opts.stop_at_first_failure else 0
    patient(lambda: board.write(request_writes(
        req_at, request_words(seq, flags, selected))), board, now)

    def taken() -> bool:
        return res.started == seq or res.done is not None

    reset_done = False
    deadline = now() + host_wait + 2
    end = now() + opts.timeout
    while not taken():
        for e in follower.until(min(deadline, end)):
            mark = res.feed(text_of(e), seq)
            if mark == "ready":
                check_image(res, opts.build_id)
            if mark == "start" and res.started == seq:
                res.clear_tests()   # lines of a run before ours (one without a request) are not ours
                break
            if mark == "done":
                break
        if taken():
            break
        if reset_done or now() >= end:
            fail(
                f"the image did not take request {seq} (no TEST|START|seq={seq})", EXIT_TIMEOUT)
        # the image took no request in its host wait (it ran everything, or never waited): the boot after a reset
        # takes it
        board.reset()
        reset_done = True
        deadline = now() + host_wait + 10
    if res.done is None:
        for e in follower.until(end):
            if res.feed(text_of(e), seq) == "done":
                break
    if res.done is None:
        fail(
            f"no TEST|DONE|...|seq={seq} within {opts.timeout:g} s", EXIT_TIMEOUT)

    final = decode_run_state(patient(lambda: board.read(
        run_at, 4 * (STATE_WORDS + 2)), board, now))
    if final is None:
        res.notes.append(
            "the run state could not be read back (CRC): verdicts from the log only")
    elif final.served_seq() != seq:
        res.notes.append(
            f"the run state serves request {final.served_seq()}, not {seq}: verdicts from the log only")
        final = None
    res.reconcile(final)
    if final is not None:
        res.boots = final.boots
    return res


def exit_code(runs: list[Results], ub: int | None, stop_at_first_failure: bool = False) -> int:
    if ub:
        return EXIT_TROUBLE
    for res in runs:
        c = res.counts()
        if res.done is None or (c["none"] and not (stop_at_first_failure and c["fail"])):
            return EXIT_TIMEOUT
    return EXIT_FAIL if any(res.counts()["fail"] for res in runs) else EXIT_OK


def run_tests(board: Board, nm: str, opts: Options, now: typing.Callable[[], float] = time.monotonic,
              out: typing.Callable[[str], None] = print) -> tuple[int, list[Results]]:
    tests, defined = tests_in(nm)
    if not tests:
        fail("the image lists no tests (no Kvasir::Test::Detail::Slot symbols): not a Kvasir::Test image")
    forgotten = unlisted(tests, defined)
    if forgotten:
        out("defined but not listed (not run): " +
            ", ".join(f"{s}.{n}" for s, n in forgotten))
    selected = select(tests, opts.filters, opts.excludes)
    t0 = now()
    if opts.flash:
        if opts.build:
            board.build()
        since = board.flash()
    else:
        since = board.mark_now()
    out(f"{'flashed through the printer; ' if opts.flash else ''}{len(tests)} tests in the image, "
        f"{len(selected)} selected")
    follower = Follower(board, since, now)
    runs = []
    seconds = []   # each run's own time, the first one with its flash
    for k in range(opts.repeat):
        start = t0 if k == 0 else now()
        runs.append(run_once(board, tests, selected, nm, opts,
                    follower, now, wait_ready=opts.flash and k == 0))
        seconds.append(now() - start)
    ub = board.ub_count() if opts.sanitize else None
    for k, res in enumerate(runs):
        if opts.repeat > 1:
            out(f"run {k + 1} of {opts.repeat}:")
        for line in table(res) + res.notes:
            out(line)
        c = res.counts()
        out(f"{c['pass']} passed, {c['fail']} failed, {c['skip']} skipped"
            + (f", {c['none']} not run" if c["none"] else "")
            + f" in {res.boots} boots, {seconds[k]:.1f} s"
            + (f"; {ub} UB reports" if ub is not None else ""))
    return exit_code(runs, ub, opts.stop_at_first_failure), runs


def run_done_line(board: Board, opts: Options, done: str, passed: str, now: typing.Callable[[], float] = time.monotonic,
                  out: typing.Callable[[str], None] = print) -> tuple[int, str | None]:
    """A self-judging image: the first line matching `done` after the mark (never one from before it: the STALE
    trap), judged by `passed`."""
    if opts.flash:
        if opts.build:
            board.build()
        since = board.flash()
    else:
        since = board.mark_now()
    done_rx, pass_rx = re.compile(done), re.compile(passed)
    line = None
    for e in Follower(board, since, now).until(now() + opts.timeout):
        if done_rx.search(text_of(e)):
            line = text_of(e)
            break
    ub = board.ub_count() if opts.sanitize else None
    if line is None:
        out(f"TIMEOUT: no line matched /{done}/ within {opts.timeout:g} s")
        return EXIT_TIMEOUT, None
    ok = bool(pass_rx.search(line))
    out(f"{'PASS' if ok else 'FAIL'}: {line}" +
        (f"; {ub} UB reports" if ub is not None else ""))
    if ub:
        return EXIT_TROUBLE, line
    return (EXIT_OK if ok else EXIT_FAIL), line


def done_line_junit(target: str, line: str | None, code: int) -> ET.ElementTree:
    root = ET.Element("testsuites", name=target)
    suite = ET.SubElement(root, "testsuite", name=target, tests="1", failures="1" if code == EXIT_FAIL else "0",
                          errors="1" if code not in (EXIT_OK, EXIT_FAIL) else "0", skipped="0")
    case = ET.SubElement(suite, "testcase", classname=target, name="done")
    if code == EXIT_FAIL:
        ET.SubElement(case, "failure", message=line or "")
    elif code != EXIT_OK:
        ET.SubElement(case, "error", message=line or "no DONE line")
    ET.SubElement(case, "system-out").text = line or ""
    return ET.ElementTree(root)


# ---- the command line -----------------------------------------------------------------------------------------------

def probe_present(probe: str, host: str, usb: list[tuple[str, str, str]]) -> bool:
    """The tree's probe on USB (serial or nickname); a network J-Link counts as there, no probe named: any."""
    if host:
        return True
    if not probe:
        return bool(usb)
    return any(probe.lstrip("0") == serial or probe == nick for serial, _, nick in usb)


def board_config(build: Path, board: str) -> tuple[Path, list[str]]:
    """--board: the tree and the skip list from the first .kvasir_test.json at or above BUILD."""
    for d in [build, *build.parents]:
        f = d / ".kvasir_test.json"
        if f.is_file():
            cfg = json.loads(f.read_text())
            boards = cfg.get("boards", {})
            if board not in boards:
                fail(
                    f"{f} names no board '{board}' (boards: {', '.join(boards) or 'none'})")
            return d / boards[board], list(cfg.get("skip", {}).get(board, []))
    fail(f"--board {board}: no .kvasir_test.json at or above {build}")


def main(argv: list[str] | None = None, usb: typing.Callable[[], list] = printer.probes_on_usb) -> int:
    p = argparse.ArgumentParser(prog="kvasir_test.py", description=__doc__.split("\n\n")[0],
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    lp = sub.add_parser(
        "list", help="the tests of the image, from its symbols")
    rp = sub.add_parser("run", help="flash, run and judge the image")
    for q in (lp, rp):
        q.add_argument("build")
        q.add_argument("target")
    rp.add_argument("--no-flash", action="store_true",
                    help="run against the image the board runs now")
    rp.add_argument("--no-build", action="store_true",
                    help="flash the image as built")
    rp.add_argument("--filter", action="append", default=[],
                    help="run only suite.name matching (glob)")
    rp.add_argument("--exclude", action="append", default=[],
                    help="leave out suite.name matching (glob)")
    rp.add_argument(
        "--board", help="BUILD is the repo; the tree and skip list from its .kvasir_test.json")
    rp.add_argument("--stop-at-first-failure", action="store_true")
    rp.add_argument("--repeat", type=int, default=1)
    rp.add_argument("--timeout", type=float, default=120)
    rp.add_argument("--junit", type=Path)
    rp.add_argument("--json", type=Path)
    rp.add_argument(
        "--done", help="a self-judging image: the regex of its DONE line")
    rp.add_argument("--pass", dest="passed", default="fail=0",
                    help="with --done: the regex a passing DONE line has")
    args = p.parse_args(argv)
    try:
        build, skips = Path(args.build), []
        if getattr(args, "board", None):
            build, skips = board_config(Path(args.build).resolve(), args.board)
        tree = trees.Tree(str(build), args.target)
        nm = tree.image.mangled_nm
        if args.cmd == "list":
            tests, defined = tests_in(nm)
            for t in tests:
                print(f"{t.index:3} {t.label}")
            forgotten = unlisted(tests, defined)
            for s, n in forgotten:
                print(
                    f"ERROR: {s}.{n} is defined but not listed (KVASIR_TEST_SUITE / Kvasir::Test::List)")
            return EXIT_FAIL if forgotten else EXIT_OK
        if not probe_present(tree.probe, tree.host, usb()):
            print(
                f"kvasir_test: probe '{tree.probe}' is not on USB: no board, skipped", file=sys.stderr)
            return EXIT_NO_BOARD
        opts = Options(flash=not args.no_flash, build=not args.no_build, filters=args.filter,
                       excludes=args.exclude + skips, stop_at_first_failure=args.stop_at_first_failure,
                       repeat=max(1, args.repeat), timeout=args.timeout, sanitize=args.target.endswith("_sanitize"),
                       build_id=image_build_id(tree))
        board = PrinterBoard(tree)
        if args.done:
            code, line = run_done_line(board, opts, args.done, args.passed)
            if args.junit:
                args.junit.parent.mkdir(parents=True, exist_ok=True)
                done_line_junit(args.target, line, code).write(
                    args.junit, encoding="unicode", xml_declaration=True)
            return code
        code, runs = run_tests(board, nm, opts)
        if args.junit:
            args.junit.parent.mkdir(parents=True, exist_ok=True)
            junit(runs, args.target).write(args.junit,
                                           encoding="unicode", xml_declaration=True)
        if args.json:
            args.json.write_text(json.dumps(as_json(runs), indent=1))
        return code
    except Outcome as o:
        print(f"kvasir_test: {o}", file=sys.stderr)
        return o.code


if __name__ == "__main__":
    sys.exit(main())
