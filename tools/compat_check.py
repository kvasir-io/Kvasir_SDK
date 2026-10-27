#!/usr/bin/env python3
"""Backward-compatibility code in the Kvasir repositories, found by its shape.

The Kvasir repositories keep no backward compatibility: no alias, alias enumerator, forwarding header, rename guard,
`[[deprecated]]` name, migration helper or "one release" shim; no version contract between the SDK and the packages;
no format or wire version word, no detection of an older file shape or frame, no "built with an older X, rebuild"
refusal and no test of one - old input fails as ordinary malformed input. A rename renames every user in the same
change. This tool finds the shapes such code takes, so that it does not come back:

  version marker     a format, protocol or record version word (`ProtocolVersion`, `FormatVersion`, `kVersion`,
                     `FORMAT_VERSION`, a `constexpr ... Version = 3`, a "protocol"/"version"/"Wire" key, an
                     `OldHeader...`-style identifier, `kvasir_requires_sdk`)
  old wording        a message or comment that explains code by an older peer: an old/older/newer printer, SDK,
                     image, firmware, format, layout, catalog, protocol, name, spelling ...; "built with an older",
                     "built before"; "rebuild" next to such words; "for backward compatibility", "compatibility
                     shim/path/alias"; "for one release"; "formerly", "(was `X`)", "is now called", "still an alias";
                     a refactor tag (`pre-5.6`); "predates the move/API"
  legacy             `Legacy` names and the word legacy (not RFC 6762's "legacy unicast" querier, not "legacy MBR")
  migration          migrate / migration / Migrate helpers and wording
  deprecated         `[[deprecated]]`, `__attribute__((deprecated))`, CMake `message(DEPRECATION`, DeprecationWarning
  rename guard       a `*_RENAMED` macro, or a static_assert / #error / CMake message / string saying a name was
                     renamed, is now called, has moved
  tool gate          a branch on an older tool: `if(POLICY ...)`, `CMAKE_VERSION VERSION_LESS`, a compiler-version
                     compare (`__clang_major__ <`), `-dumpversion`, `defined(__has_cpp_attribute)`-style guards around
                     a feature test, `sys.version_info <`
  alias              in a library header, at public namespace scope (not in a class, a function or a detail
                     namespace): a namespace alias or `using X = A::X;` under the target's own name, a
                     using-declaration (`using A::X;`, a second spelling of X), or `using X = Y;` naming a sibling of
                     the same namespace under another name
  forwarding header  a header whose only content is one #include

Files: what git lists in the repository (tracked, plus untracked files not ignored), else every file below it -
recursing into submodules only in Kvasir_SDK, which owns its submodules (any other repository's submodule is a
repository of its own and scanned as one); not through a symlinked directory, not the notebooks (CLAUDE.md), not LICENSE, not this tool and
its keep tables, not upstream code in Kvasir_SDK/lib/{libc,libcxx,compiler-rt} (only its Kvasir-owned files), not
binary or data files. The wording kinds read every text file, version markers, deprecated, rename guards and tool
gates the code files (C/C++, Python, CMake, shell), the alias and forwarding-header kinds the headers (alias: only
library repositories, outside tests).

There is no allow-list: a finding is NEW (exit 1) unless a keep table lists it (KEPT: deliberate, the reason says why
it is no compat), and a keep entry whose files were scanned but whose finding is gone is STALE (exit 1). A true
compat leftover is deleted - every user moved - not listed. Keep tables (TOML, `[keep]`):

  "<repo>/<path>: <kind>: <text>" = "reason"            one finding (<text>: the matched text, whitespace collapsed)
  "<repo>/<path>: <kind>: <text>" = ["reason", count]   that finding `count` times in the file (more is NEW)
  "<repo>/<path>: <kind>" = "reason"                    every finding of that kind in the file(s)

<path> may be a glob (`*` within a path part, `**` across parts, `{a,b}`). An empty reason, an unknown kind or
table is a table error (exit 2). The default table is tools/compat_keep.toml next to this script (library
repositories only, neutral reasons); `--overlay` merges more tables over it. An overlay's `[workspace]` names the
repositories `--root` scans: `list = "<file below root>"` (the first word of each line that names a directory below
root; `#` comments), `library = ["<name glob>", ...]` (the alias kind applies), `skip = ["<name>", ...]`.

    compat_check.py --repo NAME=PATH [--app NAME=PATH] ...   these checkouts (--repo: a library, --app: a firmware)
    compat_check.py --root <dir> --overlay <toml>            the checkouts the overlay's [workspace] names
    compat_check.py --self-test                              every pattern against its fixtures; exit 1 on a miss
    options: --keep <toml> (default tools/compat_keep.toml), --report (KEPT too, exit 0), --json, --quiet

Exit code: 0 no NEW and no STALE (or --report); 1 a NEW finding or a STALE entry; 2 a table error.
"""

from __future__ import annotations

import argparse
import bisect
import fnmatch
import json
import os
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

try:
    import tomllib
except ImportError:  # pragma: no cover
    sys.exit("compat_check.py needs Python >= 3.11 (tomllib)")

HERE = Path(__file__).resolve().parent
DEFAULT_KEEP = HERE / "compat_keep.toml"
TABLES = ("keep", "workspace")
WORKSPACE_KEYS = ("list", "library", "skip")

CPP_EXT = (".hpp", ".h", ".hh", ".hxx", ".cpp", ".cc",
           ".cxx", ".c", ".inc", ".ipp", ".tpp")
HEADER_EXT = (".hpp", ".h", ".hh", ".hxx", ".inc", ".ipp", ".tpp")
CODE_EXT = CPP_EXT + (".py", ".cmake", ".sh", ".in",
                      ".just", ".S", ".s", ".ld")
CODE_NAMES = ("CMakeLists.txt", "justfile", "libcport")
# data and binaries: never read
SKIP_EXT = {".svd", ".pdf", ".png", ".jpg", ".jpeg", ".gif", ".bmp", ".ico", ".bin", ".elf", ".uf2", ".hex", ".zip",
            ".gz", ".xz", ".tar", ".ttf", ".otf", ".bdf", ".a", ".o", ".so", ".dat", ".kicad_pcb", ".kicad_sch",
            ".kicad_pro", ".step", ".stl", ".rttlog", ".pickle"}
SKIP_NAMES = {"CLAUDE.md", "LICENSE",
              "LICENSE.txt", "LICENSE.md", "compat_check.py"}
# the one repository whose submodules are its own code (the others' submodules are top-level repositories)
OWNS_SUBMODULES = "Kvasir_SDK"
UPSTREAM = re.compile(r"^Kvasir_SDK/lib/(libc|libcxx|compiler-rt)/")
OWNED = re.compile(
    r"^Kvasir_SDK/lib/[^/]+/(.*/)?kvasir/|^Kvasir_SDK/lib/[^/]+/[^/]+\.cmake$")
TEST_PATH = re.compile(
    r"(^|/)(tests?|fuzz|fixtures?|examples?)/|(^|/)test_[^/]*$|_tests?\.\w+$")

# --- line patterns ------------------------------------------------------------------------------------------------
# (kind, scope, pattern, anchors): scope "all" (every text file) or "code"; a kind has one row per shape. A file is
# searched for a row only if its lower-case text holds one of the row's anchors, or its text one of the
# case-sensitive ones (`=Old`); the anchors keep the run short (one alternation of every shape tried at each
# position took most of it).

FORMATISH = (r"(?:Protocol|Format|Wire|Layout|Record|Schema|Cell|Store|Settings|Config|Header|Frame|Catalog|"
             r"Descriptor|Current|Learned|Calibration|Cal|Persist|Persistent|Api|API|Sdk|SDK|Image|Trace|"
             r"Ring|Hello|Stream|Old|Legacy|Compat)")
OLD_THING = (r"(?:printer|SDK|image|firmware|format|layout|catalog|protocol|header|shape|pioasm|compiler|toolchain|"
             r"libstdc\+\+|binutils|release|API|ABI|wire format|record format|frame format|name|spelling|build tree)")

LINE_PATTERNS = [
    ("version marker", "code",
     r"\b" + FORMATISH +
     r"_?Version\b|\b[A-Z0-9_]*(?:FORMAT|PROTOCOL|WIRE|LAYOUT|SCHEMA|RECORD|STORE|CONFIG|API|SDK)"
     r"_VERSION\b|\bkVersion\b|\bconstexpr[ \t]+(?:[\w:]+(?:<[^<>;\n]*>)?[ \t]+)+[Vv]ersion[ \t]*(?:=|\{)[ \t]*\d",
     ["version"]),
    ("version marker", "code",
     r"[\"'](?:(?:[Pp]rotocol|[Vv]ersion|[Ww]ire)(?:_?[Vv]ersion)?|[Ff]ormat_?[Vv]ersion)[\"'][ \t]*[:\]]"
     r"|\.get\(\s*[\"'](?:protocol|version)[\"']",
     ["protocol\"", "protocol'", "version\"", "version'", "wire\"", "wire'"]),
    ("version marker", "code",
     r"(?:Old|Legacy|Compat)(?:Header|Format|Layout|Version|Protocol|Record|Frame|Shape|Name|Api|Wire|Catalog|Size|"
     r"Magic)\w*",
     ["=Old", "=Legacy", "=Compat"]),
    ("version marker", "code", r"\bkvasir_requires_\w+|\bKVASIR_(?:PORT_)?API_VERSION\b",
     ["kvasir_requires", "api_version"]),
    ("old wording", "all", r"(?i:\b(?:old|older|newer)\s+" +
     OLD_THING + r"s?\b)", ["old ", "old\n", "older", "newer"]),
    ("old wording", "all",
     r"(?i:\b(?:built|compiled|configured|linked|flashed)\s+(?:with|by|from|before)\s+(?:an?\s+|the\s+)?"
     r"(?:older|old|earlier|previous)\b"
     r"|\b(?:built|compiled|configured|linked)\s+before\s+(?:it|its|[\w`]+\s+(?:existed|had|was|were|came))\b)",
     ["built", "compiled", "configured", "linked", "flashed"]),
    ("old wording", "all",
     r"(?i:\brebuild\b[^.\n]*\b(?:older|old|another version|mismatch|speaks)\b"
     r"|\b(?:older|old|another version|mismatch|speaks)\b[^.\n]*\brebuild\b)",
     ["rebuild"]),
    ("old wording", "all",
     r"(?i:\b(?:backwards?|forwards?)[- ]compat(?:ible|ibility)?\b"
     r"|\bfor\s+(?:API\s+|ABI\s+|source\s+|binary\s+)?compat(?:ibility)?\b"
     r"|\bcompat(?:ibility)?\s+(?:shim|alias|layer|header|wrapper|mode|path|branch|code|guard|flag|note)s?\b)",
     ["compat"]),
    ("old wording", "all", r"(?i:\b(?:for|kept|keep|keeps|until the next)\s+one\s+(?:more\s+)?release\b)",
     ["release"]),
    ("old wording", "all", r"(?i:\bformerly\b|\bpreviously\s+(?:called|named|spelled)\b)",
     ["formerly", "previously"]),
    ("old wording", "all",
     r"\b(?:was|were)\s+(?:called|named|spelled)\s+[`'\"]|\b(?:is|are)\s+now\s+(?:called|named|spelled)\b"
     r"|\bspelled\s+[`'][^`'\n]+[`']\s+until\b",
     ["called", "named", "spelled"]),
    ("old wording", "all", r"\brenamed\s+(?:from|to)\s+[`'\"]", ["renamed"]),
    ("old wording", "all",
     r"\((?:was|formerly)\s+[`'][^`'\n]+[`']\)", ["(was", "(formerly"]),
    ("old wording", "all", r"\bstill\s+an\s+alias\b", ["still an"]),
    ("old wording", "all", r"\bpre-\d+\.\d+[a-z]?\b", ["pre-"]),
    ("old wording", "all",
     r"\bpredates?\s+(?:the|that|this|its)\s+(?:\w+\s+){0,2}(?:move|rename|split|API|change|refactor|form|shape|"
     r"interface)\b",
     ["predate"]),
    ("legacy", "all",
     r"Legacy\w*|\blegacy\b(?![ -](?:unicast|quer|resolver|MBR))", ["legacy"]),
    ("migration", "all",
     r"[Mm]igrat(?:e|es|ed|ing|ion|ions)\b|Migrat\w*", ["migrat"]),
    ("deprecated", "code",
     r"\[\[\s*deprecated\b|__attribute__\s*\(\(\s*deprecated|\bmessage\s*\(\s*DEPRECATION\b|\bDeprecationWarning\b",
     ["deprecat"]),
    ("rename guard", "code", r"\b[A-Z][A-Z0-9_]*_RENAMED\b", ["_renamed"]),
    ("rename guard", "code",
     r"(?:\bstatic_assert|#[ \t]*error|#[ \t]*warning|\bmessage[ \t]*\([ \t]*(?:FATAL_ERROR|SEND_ERROR|WARNING|"
     r"AUTHOR_WARNING))\b.*\b(?:renamed|is now (?:called|named|spelled)|has moved|moved to|"
     r"no longer (?:exists|called|named))\b"
     r"|\"[^\"\n]*\b(?:is now (?:called|named|spelled)|was renamed|has been renamed|renamed to|has moved to|"
     r"no longer exists)\b[^\"\n]*\"",
     ["renamed", "now called", "now named", "now spelled", "moved", "no longer"]),
    ("tool gate", "code", r"\bif\s*\(\s*(?:NOT\s+)?POLICY\b", ["policy"]),
    ("tool gate", "code",
     r"\b(?:CMAKE_VERSION|CMAKE_\w+_COMPILER_VERSION)\s+VERSION_(?:LESS|GREATER)\w*|-dumpversion\b"
     r"|\bsys\.version_info\s*[<>]",
     ["version_less", "version_greater", "dumpversion", "version_info"]),
    ("tool gate", "code",
     r"\b__clang_major__\s*[<>]|\b__GNUC__\s*[<>]", ["__clang_major__", "__gnuc__"]),
    ("tool gate", "code", r"\bdefined\s*\(\s*__has_(?:cpp_attribute|embed|builtin|include|attribute)\s*\)",
     ["defined"]),
]

STRUCTURAL = [("alias", "library header"), ("forwarding header", "header")]
KINDS = list(dict.fromkeys(k for k, *_ in LINE_PATTERNS)) + \
    [k for k, _ in STRUCTURAL]


def is_code(path: str) -> bool:
    name = path.rsplit("/", 1)[-1]
    return name in CODE_NAMES or name.endswith(CODE_EXT)


def is_header(path: str) -> bool:
    return path.endswith(HEADER_EXT)


# --- C++ text: comments and strings blanked, namespace scope ------------------------------------------------------

def strip_cpp(text: str) -> str:
    """Comments and the contents of string and char literals become spaces; newlines stay (line numbers)."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif c == "/" and text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", text[i:j]))
            i = j
        elif c == "R" and text.startswith('R"', i) and (i == 0 or not (text[i - 1].isalnum() or text[i - 1] == "_")):
            m = re.match(r'R"([^ ()\\\n]{0,16})\(', text[i:])
            if not m:
                out.append(c)
                i += 1
                continue
            end = text.find(")" + m.group(1) + '"', i + m.end())
            end = n if end < 0 else end + len(m.group(1)) + 2
            out.append(re.sub(r"[^\n]", " ", text[i:end]))
            i = end
        elif c in "\"'":
            # digit separator 1'000
            if c == "'" and i > 0 and text[i - 1].isalnum():
                out.append(c)
                i += 1
                continue
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            out.append(
                c + re.sub(r"[^\n]", " ", text[i + 1:j - 1]) + (text[j - 1] if j - 1 > i else ""))
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


BRACE = re.compile(r"(?P<ns>\bnamespace\s+(?P<name>[A-Za-z_]\w*(?:\s*::\s*(?:inline\s+)?[A-Za-z_]\w*)*)?\s*\{)"
                   r"|(?P<ext>\bextern\s+\"C(?:\+\+)?\"\s*\{)|(?P<open>\{)|(?P<close>\})")
NS_ALIAS = re.compile(r"\bnamespace\s+(?P<name>[A-Za-z_]\w*)\s*=\s*(?P<target>(?:::\s*)?[A-Za-z_]\w*"
                      r"(?:\s*::\s*[A-Za-z_]\w*)*)\s*;")
USING_ALIAS = re.compile(r"\busing\s+(?P<name>[A-Za-z_]\w*)\s*(?:\[\[[^\]]*\]\]\s*)?=\s*(?P<target>(?:::\s*)?"
                         r"[A-Za-z_]\w*(?:\s*::\s*[A-Za-z_]\w*)*)\s*;")
USING_DECL = re.compile(
    r"\busing\s+(?!namespace\b)(?P<target>(?:::\s*)?[A-Za-z_]\w*(?:\s*::\s*[A-Za-z_]\w*)+)\s*;")
BUILTIN = {"bool", "char", "short", "int", "long",
           "unsigned", "signed", "float", "double", "void", "auto"}


def public_scopes(code: str) -> list[tuple[int, int]]:
    """Spans of `code` at public namespace scope: inside namespaces only (or none), none of them a detail one."""
    spans, stack, last = [], [], 0

    def public() -> bool:
        return all(f is not None and not f.lower().endswith("detail") for f in stack)

    for m in BRACE.finditer(code):
        if public():
            spans.append((last, m.start()))
        if m.group("ns"):
            parts = [p.strip().removeprefix("inline").strip()
                     for p in (m.group("name") or "").split("::")]
            stack.append(parts[-1] if parts and parts[-1] else "")
        elif m.group("ext"):
            stack.append("")
        elif m.group("open"):
            stack.append(None)
        elif stack:
            stack.pop()
        last = m.end()
    if public():
        spans.append((last, len(code)))
    return spans


def last_part(name: str) -> str:
    return re.split(r"\s*::\s*", name.strip())[-1]


def alias_findings(text: str) -> list[tuple[int, str]]:
    """(offset, text) of every alias that only renames, at public namespace scope."""
    code = strip_cpp(text)
    out = []
    for start, end in public_scopes(code):
        piece = code[start:end]
        for rx in (NS_ALIAS, USING_ALIAS, USING_DECL):
            for m in rx.finditer(piece):
                before = piece[:m.start()].rstrip()
                # an alias template adds parameters
                if re.search(r"template\s*<[^;{}]*>$", before):
                    continue
                target = re.sub(r"\s+", "", m.group("target"))
                if rx is USING_DECL:
                    # a re-export: a second spelling of its last part
                    pass
                elif rx is NS_ALIAS:
                    if m.group("name").lower() != last_part(target).lower():
                        # an abbreviation (`namespace P = Peripheral`)
                        continue
                else:
                    if target.lstrip(":").startswith("std::") or target in BUILTIN:
                        continue
                    same = m.group("name") == last_part(target)
                    sibling = "::" not in target
                    if not (same or sibling):
                        continue                                  # a name for a qualified entity elsewhere
                out.append((start + m.start(), " ".join(m.group(0).split())))
    return out


INCLUDE_LINE = re.compile(r"\s*#\s*include\b")
GUARD_LINE = re.compile(
    r"#\s*(?:pragma\s+once|ifndef\s+\w+|define\s+\w+|endif\b.*)")


def forwarding_finding(text: str) -> tuple[int, str] | None:
    """A header whose only content is one #include (besides #pragma once / an include guard)."""
    lines = [(n, l.strip()) for n, l in enumerate(
        strip_cpp(text).splitlines(), 1) if l.strip()]
    body = [(n, l) for n, l in lines if not GUARD_LINE.fullmatch(l)]
    if len(body) == 1 and INCLUDE_LINE.match(body[0][1]):
        raw = text.splitlines()[body[0][0] - 1].strip()
        return body[0][0], " ".join(raw.split())
    return None


# --- scanning -----------------------------------------------------------------------------------------------------

@dataclass
class Finding:
    path: str    # <repo>/<path>
    line: int
    kind: str
    text: str


class Scanner:
    def __init__(self):
        self.line_patterns = [(k, scope, re.compile(p, re.M), anchors)
                              for k, scope, p, anchors in LINE_PATTERNS]

    def scan(self, path: str, text: str, library: bool) -> list[Finding]:
        low = text.lower()
        starts = [0] + [m.end() for m in re.finditer("\n", text)]
        code = is_code(path)
        out: list[Finding] = []
        seen_lines: set[tuple[int, str]] = set()
        for kind, scope, rx, anchors in self.line_patterns:
            if scope == "code" and not code:
                continue
            if anchors is not None and not any(text.find(a[1:]) >= 0 if a.startswith("=") else a in low
                                               for a in anchors):
                continue
            for m in rx.finditer(text):
                line = bisect.bisect_right(starts, m.start())
                if (line, kind) in seen_lines:   # one finding per kind and line
                    continue
                seen_lines.add((line, kind))
                out.append(Finding(path, line, kind,
                           " ".join(m.group(0).split())))
        rel = path.split("/", 1)[1] if "/" in path else path
        if is_header(path):
            if library and not TEST_PATH.search(rel) and ("using" in text or "namespace" in text):
                for pos, t in alias_findings(text):
                    out.append(
                        Finding(path, bisect.bisect_right(starts, pos), "alias", t))
            fw = None if TEST_PATH.search(rel) else forwarding_finding(text)
            if fw:
                out.append(Finding(path, fw[0], "forwarding header", fw[1]))
        return sorted(out, key=lambda f: (f.line, f.kind))


def listed_files(root: Path, submodules: bool) -> list[str]:
    """Paths relative to root: git's list (tracked, through submodules when asked, plus untracked not ignored), else
    a walk."""
    def ls(*opts):
        return subprocess.run(["git", "-C", str(root), "ls-files", "-z", *opts], capture_output=True, text=True)
    tracked = ls("--recurse-submodules") if submodules else ls()
    if tracked.returncode == 0:
        # git refuses -o together with --recurse-submodules: tracked files of every submodule, untracked of the top
        names = tracked.stdout.split(
            "\0") + ls("-o", "--exclude-standard").stdout.split("\0")
        return sorted({n for n in names if n})
    out = []
    for dp, dn, fn in os.walk(root, followlinks=False):
        dn[:] = sorted(d for d in dn if d not in (
            ".git", "_deps") and not d.startswith("build"))
        out += [os.path.relpath(os.path.join(dp, f),
                                root).replace(os.sep, "/") for f in fn]
    return sorted(out)


def repo_files(name: str, root: Path) -> list[tuple[str, Path]]:
    root = Path(os.path.abspath(root))
    real_root = os.path.realpath(root)
    out = []
    for rel in listed_files(root, submodules=name == OWNS_SUBMODULES):
        f = root / rel
        key = f"{name}/{rel}"
        if f.name in SKIP_NAMES or f.name.startswith("compat_keep") or f.suffix.lower() in SKIP_EXT:
            continue
        if UPSTREAM.match(key) and not OWNED.match(key):
            continue
        if f.is_symlink() or not f.is_file() or os.path.realpath(f) != os.path.join(real_root, rel):
            continue   # a linked file or a path through a symlinked directory: its own repository scans it
        out.append((key, f))
    return out


# --- keep tables --------------------------------------------------------------------------------------------------

class TableError(Exception):
    pass


def glob_regex(pattern: str) -> re.Pattern:
    """`**` any path (`**/` zero or more directories), `*` and `?` within one path part, `{a,b}`."""
    out, i = [], 0
    while i < len(pattern):
        c = pattern[i]
        if pattern.startswith("**/", i):
            out.append("(?:.*/)?")
            i += 3
        elif pattern.startswith("**", i):
            out.append(".*")
            i += 2
        elif c == "*":
            out.append("[^/]*")
            i += 1
        elif c == "?":
            out.append("[^/]")
            i += 1
        elif c == "{":
            j = pattern.index("}", i)
            out.append("(?:" + "|".join(re.escape(p)
                       for p in pattern[i + 1:j].split(",")) + ")")
            i = j + 1
        else:
            out.append(re.escape(c))
            i += 1
    return re.compile("".join(out) + r"\Z")


@dataclass
class Entry:
    key: str
    path: re.Pattern
    kind: str
    text: str | None
    reason: str
    count: int
    glob: bool


def load_tables(keep: Path, overlays: list[Path]) -> dict:
    def load(p: Path) -> dict:
        data = tomllib.loads(p.read_text())
        unknown = sorted(set(data) - set(TABLES))
        if unknown:
            raise TableError(f"{p.name}: unknown table(s) {', '.join(f'[{t}]' for t in unknown)}: the tables are "
                             + ", ".join(f"[{t}]" for t in TABLES))
        return data
    data = load(keep)
    for o in overlays:
        extra = load(o)
        data.setdefault("keep", {}).update(extra.get("keep", {}))
        if "workspace" in extra:
            data["workspace"] = extra["workspace"]
    return data


def entries_of(keep: dict) -> list[Entry]:
    out = []
    for key, value in keep.items():
        reason, count = value, 1
        if isinstance(value, list):
            if len(value) != 2 or not isinstance(value[1], int) or isinstance(value[1], bool) or value[1] < 2:
                raise TableError(
                    f"[keep] {key!r}: [\"reason\", count] with a count >= 2")
            reason, count = value
        if not isinstance(reason, str) or not reason.strip():
            raise TableError(f"[keep] {key!r}: needs a reason")
        parts = key.split(": ", 2)
        if len(parts) < 2 or parts[1] not in KINDS:
            raise TableError(f"[keep] {key!r}: \"<repo>/<path>: <kind>[: <text>]\" with a kind of "
                             + ", ".join(KINDS))
        if len(parts) == 2 and count != 1:
            raise TableError(
                f"[keep] {key!r}: a count needs the finding's text")
        out.append(Entry(key, glob_regex(parts[0]), parts[1], parts[2] if len(parts) == 3 else None, reason, count,
                         any(c in parts[0] for c in "*?{")))
    return out


def judge(findings: list[Finding], entries: list[Entry], scanned: set[str]) -> dict:
    """NEW / KEPT / STALE as layer_check does: an exact entry takes up to `count` findings of its text in each file it
    names, a kind entry every finding of its kind in its files. STALE: an entry that took no finding, or a one-file
    exact entry that took fewer than its count."""
    used: dict[str, int] = {}
    per_file: dict[tuple[str, str], int] = {}
    result = {"new": [], "kept": [], "stale": []}
    exact = [e for e in entries if e.text is not None]
    whole = [e for e in entries if e.text is None]
    for f in findings:
        row = {"path": f.path, "line": f.line, "kind": f.kind, "text": f.text,
               "key": f"{f.path}: {f.kind}: {f.text}"}
        e = next((e for e in exact if e.kind == f.kind and e.text ==
                 f.text and e.path.match(f.path)), None)
        if e is not None and per_file.get((e.key, f.path), 0) < e.count:
            per_file[(e.key, f.path)] = per_file.get((e.key, f.path), 0) + 1
            used[e.key] = used.get(e.key, 0) + 1
            result["kept"].append(row | {"reason": e.reason})
            continue
        if e is not None:
            row["why"] = f"more than the {e.count} the keep entry counts"
            result["new"].append(row)
            continue
        e = next((e for e in whole if e.kind ==
                 f.kind and e.path.match(f.path)), None)
        if e is not None:
            used[e.key] = used.get(e.key, 0) + 1
            result["kept"].append(row | {"reason": e.reason})
            continue
        result["new"].append(row)
    for e in entries:
        if not any(e.path.match(p) for p in scanned):
            continue   # its repository was not scanned
        n = used.get(e.key, 0)
        if n == 0 or (n < e.count and not e.glob):
            why = "no finding left" if n == 0 else f"{n} finding(s), the entry counts {e.count}"
            result["stale"].append(
                {"key": e.key, "reason": e.reason, "why": why})
    return result


# --- workspace ------------------------------------------------------------------------------------------------------

def workspace_repos(root: Path, ws: dict) -> list[tuple[str, Path, bool]]:
    unknown = set(ws) - set(WORKSPACE_KEYS)
    if unknown:
        raise TableError(f"[workspace]: unknown key(s) {', '.join(sorted(unknown))}: the keys are "
                         + ", ".join(WORKSPACE_KEYS))
    if "list" not in ws:
        raise TableError(
            "[workspace]: `list` names the file below --root that lists the checkouts")
    listing = root / ws["list"]
    if not listing.is_file():
        raise TableError(f"[workspace] list: {listing} is no file")
    library, skip = ws.get("library", []), set(ws.get("skip", []))
    names = []
    for line in listing.read_text().splitlines():
        words = line.split()
        if not words or words[0].startswith("#") or words[0] in skip or words[0] in names:
            continue
        if (root / words[0]).is_dir() and not (root / words[0]).is_symlink():
            names.append(words[0])
    return [(n, root / n, any(fnmatch.fnmatchcase(n, g) for g in library)) for n in names]


# --- self-test ------------------------------------------------------------------------------------------------------

# (path, text, library, kinds it must report): a hit and a near miss for every kind; a path without "/" means
# "x/<path>" (a library file in repository x)
FIXTURES = [
    # version marker
    ("src/a.hpp", "static constexpr std::uint16_t ProtocolVersion = 12;",
     True, ["version marker"]),
    ("src/a.hpp",
     "inline constexpr std::uint32_t FormatVersion{1};", True, ["version marker"]),
    ("src/a.hpp", "static constexpr auto kVersion = 3;",
     True, ["version marker"]),
    ("src/a.cc", "#define DATAFILE_FORMAT_VERSION 1",
     True, ["version marker"]),
    ("src/a.hpp", "    static constexpr std::uint32_t Version = 3;",
     True, ["version marker"]),
    ("src/a.hpp",
     "static constexpr std::uint16_t CurrentVersion{2};", True, ["version marker"]),
    ("tools/a.py", "if answer[\"protocol\"] != 2:", True, ["version marker"]),
    ("src/a.cpp", "out << R\"({\"Wire\": 3})\";", True, ["version marker"]),
    ("tools/a.py", "v = hello.get(\"version\")", True, ["version marker"]),
    ("src/a.hpp", "constexpr std::size_t TraceOldHeaderSize = 8;",
     True, ["version marker"]),
    ("cmake/a.cmake", "kvasir_requires_sdk(3)", True, ["version marker"]),
    ("src/a.hpp", ".bcdDevice{DeviceVersion},", True, []),
    ("src/a.hpp", "static constexpr std::uint32_t WindowsVersion = 0x06030000;", True, []),
    ("src/a.hpp",
     "static constexpr Version Version{.major = __clang_major__,", True, []),
    ("src/a.hpp", "    Version         = 0x48,", True, []),
    ("src/a.hpp", "constexpr std::uint32_t HeaderVersion1 = 0x8000'0000U;", True, []),
    ("README.md", "the ProtocolVersion word went", True, []),
    ("tools/a.py", "for cmd in (\"status\", \"version\", \"build\"):", True, []),
    # old wording
    ("tools/a.py", "die('an older printer: rebuild uc_log_printer')",
     True, ["old wording"]),
    ("README.md", "Images built with an older SDK show no channel names.",
     True, ["old wording"]),
    ("src/a.hpp", "// a catalog from the old format has no FrameCheck",
     True, ["old wording"]),
    ("src/a.hpp", "// configured before printer_args existed",
     True, ["old wording"]),
    ("src/a.hpp", "// kept for backward compatibility", True, ["old wording"]),
    ("src/a.hpp", "// a compatibility shim for the old callers",
     True, ["old wording"]),
    ("src/a.hpp", "// still here for one release", True, ["old wording"]),
    ("src/a.hpp", "/// formerly Kvasir::Clocks::Clk", True, ["old wording"]),
    ("README.md", "`Hal::ClockMode` (was `SPI::Mode`)", True, ["old wording"]),
    ("src/a.hpp", "// RuntimeHook is now called Detail::RuntimeHook",
     True, ["old wording"]),
    ("src/a.hpp", "// still an alias of MPL::Dedup", True, ["old wording"]),
    ("src/a.hpp", "// the pre-5.6c register shape", True, ["old wording"]),
    ("src/a.hpp", "// this driver predates that move", True, ["old wording"]),
    ("src/a.hpp", "// renamed to `LineRecovery`", True, ["old wording"]),
    ("src/a.hpp", "// a record older than one already seen", True, []),
    ("src/a.hpp", "// RFC 792 predates the field", True, []),
    ("src/a.hpp", "// rebuild aglio with -DAGLIO_USE_ENCHANTUM=OFF", True, []),
    ("src/a.hpp", "// 512 for maximum compatibility (fatgen:193)", True, []),
    ("src/a.hpp", "// the boot's pre-PLL timing", True, []),
    ("src/a.hpp", "// setupEndpoint() was called", True, []),
    ("src/a.hpp", "// an earlier firmware may have left REG_BANK at 1; the previous image's core 1", True, []),
    ("src/a.hpp", "// waits for a release of the buffer", True, []),
    ("cmake/a.cmake", "list(APPEND flags -D_LIBCPP_ABI_VERSION=2)", True, []),
    # legacy
    ("tests/a.cpp", "auto const old = Legacy::calcBaudRegs(clk, baud);",
     True, ["legacy"]),
    ("src/a.hpp", "// the legacy layout of the first channels",
     True, ["legacy"]),
    ("src/a.hpp", "// answer a legacy querier (RFC 6762 6.7) with its id; legacy unicast", True, []),
    ("src/a.hpp", "// ---- the legacy MBR in front of a partitioned card", True, []),
    # migration
    ("src/a.hpp", "static Cell migrate(PayloadReader r);",
     True, ["migration"]),
    ("src/a.hpp", "template<typename C> concept HasMigrate = true;",
     True, ["migration"]),
    ("src/a.hpp", "// the immigrant count", True, []),
    # deprecated
    ("src/a.hpp", "[[deprecated(\"use Hal::Clk\")]] using Clk = Hal::Clk;",
     True, ["alias", "deprecated"]),
    ("src/a.hpp", "void f() __attribute__((deprecated));",
     True, ["deprecated"]),
    ("cmake/a.cmake", "message(DEPRECATION \"use VARIANT\")",
     True, ["deprecated"]),
    ("src/a.hpp",
     "bool deprecated{false};   // RFC 4862: past its preferred lifetime", True, []),
    # rename guard
    ("src/a.hpp", "#define KVASIR_CONFIG_RENAMED(old, new) static_assert(!requires { old; })", True,
     ["rename guard"]),
    ("src/a.hpp", "static_assert(!HasOld<C>, \"Config::maxGap is now called maxPause\");", True,
     ["old wording", "rename guard"]),
    ("cmake/a.cmake", "message(FATAL_ERROR \"USE_LOG was renamed: VARIANT\")",
     True, ["rename guard"]),
    ("tests/a.cpp", "check(v.stat(\"/renamed.bin\").has_value(), \"the renamed file\");", True, []),
    # tool gate
    ("cmake/a.cmake", "if(POLICY CMP0174)", True, ["tool gate"]),
    ("CMakeLists.txt", "if(CMAKE_VERSION VERSION_LESS 3.19)",
     True, ["tool gate"]),
    ("src/a.hpp", "#if __clang_major__ >= 23", True, ["tool gate"]),
    ("cmake/a.cmake",
     "execute_process(COMMAND ${CMAKE_CXX_COMPILER} -dumpversion)", True, ["tool gate"]),
    ("src/a.hpp", "#if defined(__has_cpp_attribute)", True, ["tool gate"]),
    ("tools/a.py", "if sys.version_info < (3, 11):", True, ["tool gate"]),
    ("src/a.hpp", "#if defined(__has_feature) && __has_feature(address_sanitizer)", True, []),
    ("src/a.hpp", "#if __has_cpp_attribute(clang::lifetimebound)", True, []),
    ("CMakeLists.txt", "cmake_minimum_required(VERSION 3.28)", True, []),
    # alias (library headers, public namespace scope)
    ("src/a.hpp",
     "namespace Kvasir::I2C { namespace Units = ::Kvasir::Units; }", True, ["alias"]),
    ("src/a.hpp",
     "namespace Kvasir { namespace Net = ::net; }", True, ["alias"]),
    ("src/a.hpp",
     "namespace Kvasir::Nvm { using Retry = Kvasir::Retry; }", True, ["alias"]),
    ("src/a.hpp",
     "namespace net { using NetCounters = Counters; }", True, ["alias"]),
    ("src/a.hpp",
     "namespace Kvasir::Chip { using Kvasir::Register::MicrosecondsBound; }", True, ["alias"]),
    ("src/a.hpp", "namespace Kvasir { namespace P = Peripheral; }", True, []),
    ("src/a.hpp",
     "namespace Kvasir { using Pin = Io::PinLocation<0, 1>; }", True, []),
    ("src/a.hpp",
     "namespace Kvasir { template<typename T> using Of = Detail::Of; }", True, []),
    ("src/a.hpp",
     "namespace Kvasir { using Word = std::uint32_t; }", True, []),
    ("src/a.hpp",
     "namespace Kvasir { namespace detail { using Kvasir::Io::pinNumber; } }", True, []),
    ("src/a.hpp",
     "namespace Kvasir { struct S { using type = Other; }; }", True, []),
    ("src/a.hpp",
     "inline void f() { using Kvasir::Io::pinNumber; namespace R = Kvasir::Register; }", True, []),
    ("src/a.hpp", "namespace Kvasir { using Clk = Hal::Clk; }", False, []),
    ("tests/a.hpp",
     "namespace Kvasir { using Retry = Kvasir::Retry; }", True, []),
    # forwarding header
    ("src/old/Name.hpp", "#pragma once\n// moved\n#include \"kvasir/Port/Name.hpp\"\n",
     False, ["forwarding header"]),
    ("src/two.hpp", "#pragma once\n#include \"a.hpp\"\n#include \"b.hpp\"\n", False, []),
    ("src/one.hpp",
     "#pragma once\n#include \"a.hpp\"\nnamespace X { inline int f() { return 1; } }\n", False, []),
    ("src/one.cpp", "#include \"a.hpp\"\n", False, []),
    ("tests/fixture/c.hpp", "#pragma once\n#include \"high/c.hpp\"\n", True, []),
]


def self_test() -> int:
    ok = True
    scanner = Scanner()
    for path, text, library, want in FIXTURES:
        full = path if path.startswith("x/") else f"x/{path}"
        got = sorted({f.kind for f in scanner.scan(full, text, library)})
        if got != sorted(want):
            print(
                f"self-test FAILED: {full}: {text!r}: {got}, want {sorted(want)}")
            ok = False
    untested = set(KINDS) - {k for *_, want in FIXTURES for k in want}
    if untested:
        print(
            f"self-test FAILED: a kind without a hit fixture: {sorted(untested)}")
        ok = False
    # the keep table: an exact entry, its count, a kind entry, a glob, a stale entry, the refusals
    f = [Finding("x/a.hpp", 1, "legacy", "Legacy"), Finding("x/a.hpp", 2, "legacy", "Legacy"),
         Finding("x/b.hpp", 3, "alias", "using A::B;"), Finding("y/c.hpp", 4, "forwarding header", "#include \"d\"")]
    keep = {"x/a.hpp: legacy: Legacy": "an oracle", "x/*.hpp: alias": "a re-export",
            "y/c.hpp: forwarding header": "the per-core spelling", "x/gone.hpp: legacy": "stale",
            "z/never.hpp: legacy": "not scanned"}
    r = judge(f, entries_of(keep), {"x/a.hpp",
              "x/b.hpp", "y/c.hpp", "x/gone.hpp"})
    want = ({"x/a.hpp:2"}, {"x/a.hpp:1", "x/b.hpp:3",
            "y/c.hpp:4"}, {"x/gone.hpp: legacy"})
    got = ({f"{e['path']}:{e['line']}" for e in r["new"]}, {f"{e['path']}:{e['line']}" for e in r["kept"]},
           {e["key"] for e in r["stale"]})
    if got != want:
        print(
            f"self-test FAILED: keep table: new/kept/stale {got}, want {want}")
        ok = False
    r = judge(f[:1], entries_of(
        {"x/a.hpp: legacy: Legacy": ["an oracle", 2]}), {"x/a.hpp"})
    if [e["key"] for e in r["stale"]] != ["x/a.hpp: legacy: Legacy"]:
        print(
            f"self-test FAILED: a count above the findings is not stale: {r['stale']}")
        ok = False
    # a glob's exact entry counts per file
    g = [Finding("x/a.hpp", 1, "legacy", "Legacy"), Finding("x/b.hpp", 1, "legacy", "Legacy"),
         Finding("x/b.hpp", 2, "legacy", "Legacy")]
    r = judge(g, entries_of(
        {"x/*.hpp: legacy: Legacy": "an oracle"}), {"x/a.hpp", "x/b.hpp", "x/c.hpp"})
    if [f"{e['path']}:{e['line']}" for e in r["new"]] != ["x/b.hpp:2"] or r["stale"]:
        print(
            f"self-test FAILED: a glob entry's count per file: new {r['new']}, stale {r['stale']}")
        ok = False
    for bad in ({"x/a.hpp: legacy": ""}, {"x/a.hpp: oldness": "why"}, {"x/a.hpp": "why"},
                {"x/a.hpp: legacy": ["why", 2]}, {"x/a.hpp: legacy: Legacy": ["why", 1]}):
        try:
            entries_of(bad)
            print(f"self-test FAILED: no table error for {bad}")
            ok = False
        except TableError:
            pass
    print("self-test", "ok" if ok else "FAILED")
    return 0 if ok else 1


# --- main -----------------------------------------------------------------------------------------------------------

def print_report(res: dict, report: bool, quiet: bool) -> None:
    if not quiet:
        print("scanned: " +
              ", ".join(f"{k} ({v} files)" for k, v in sorted(res["scanned"].items())))
    for label in (("kept",) if report else ()) + ("new", "stale"):
        print(f"\n{label.upper()}:")
        for e in res[label]:
            if label == "stale":
                print(
                    f"  {e['key']}   ({e['why']}: remove or lower the entry)\n      reason: {e['reason']}")
                continue
            line = f"  {e['path']}:{e['line']}: {e['kind']}: {e['text']}"
            if "why" in e:
                line += f"   ({e['why']})"
            if label == "kept":
                line += f"\n      reason: {e['reason']}"
            print(line)
        if not res[label]:
            print("  -")
    per = ", ".join(f"{k} {v}" for k, v in sorted(
        res["new_per_repo"].items())) or "-"
    print(
        f"\nsummary: {len(res['new'])} new ({per}), {len(res['stale'])} stale; {len(res['kept'])} kept")


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--keep", type=Path, default=DEFAULT_KEEP)
    ap.add_argument("--overlay", type=Path, action="append", default=[])
    ap.add_argument("--repo", action="append", default=[], metavar="NAME=PATH")
    ap.add_argument("--app", action="append", default=[], metavar="NAME=PATH")
    ap.add_argument("--root", type=Path)
    ap.add_argument("--report", action="store_true", help="KEPT too; exit 0")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args(argv)
    if a.self_test:
        return self_test()
    try:
        data = load_tables(a.keep, a.overlay)
        entries = entries_of(data.get("keep", {}))
        repos: list[tuple[str, Path, bool]] = []
        for specs, library in ((a.repo, True), (a.app, False)):
            for spec in specs:
                name, sep, path = spec.partition("=")
                if not sep:
                    ap.error(f"{spec!r}: NAME=PATH")
                repos.append((name, Path(path), library))
        if a.root is not None:
            if "workspace" not in data:
                raise TableError(
                    "--root needs an overlay with a [workspace] table")
            repos += workspace_repos(a.root, data["workspace"])
        if not repos:
            ap.error("nothing to scan: --repo, --app or --root")
    except (TableError, tomllib.TOMLDecodeError, OSError) as e:
        print(f"TABLE ERROR: {e}", file=sys.stderr)
        return 2
    scanner = Scanner()
    findings: list[Finding] = []
    scanned: dict[str, int] = {}
    paths: set[str] = set()
    for name, root, library in repos:
        files = repo_files(name, root)
        scanned[name] = len(files)
        for key, f in files:
            paths.add(key)
            try:
                text = f.read_text()
            except (UnicodeDecodeError, OSError):
                continue
            findings += scanner.scan(key, text, library)
    res = judge(findings, entries, paths)
    res["scanned"] = scanned
    res["new_per_repo"] = {}
    for e in res["new"]:
        repo = e["path"].split("/", 1)[0]
        res["new_per_repo"][repo] = res["new_per_repo"].get(repo, 0) + 1
    if a.json:
        print(json.dumps(res, indent=1))
    else:
        print_report(res, a.report, a.quiet)
    if not a.report and (res["new"] or res["stale"]):
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
