#!/usr/bin/env python3
"""The include layering of the Kvasir repositories, and their raw memory accesses, checked against a table.

The layer table (tools/layers.toml next to this script) assigns every library header to a layer by repository and
path glob, and orders the layers from the bottom (base) to the top (app). Five checks run over the scanned
repositories:

  (a) include   an #include whose target is in a higher layer than the including file ("upward"), an include of
                the log libraries from a layer with `log = false`, or of a log adapter from any library layer;
  (b) detail    a name in a `detail`/`Detail` namespace that one repository declares and another one uses;
  (c) has_include  a `__has_include` in a layer with `has_include = false` (a feature switched by what happens to be
                on the include path);
  (d) mmio      an access around the register layer, in any layer: R1 a volatile pointer cast (`reinterpret_cast` /
                `static_cast` to a `volatile*`, or a C-style `(T volatile*)`), R2 a masked whole-register read
                (`readWhole(R) & m`, `m | readWhole(R)`) or a hand-written read-modify-write (`writeWhole(R,
                readWhole...`), R3 a literal register address (`Register::Address<0x...>`), R4 any other hand-spelled
                register type (`Register::Address<` or `Register::Action<`: the generated headers and the factories
                build them; a kept entry is a partial specialisation that matches the factories' types, e.g. a
                Startup resource or interrupt of a field's action). A register goes through
                the field API (apply), memory that is not a register through Kvasir::Memory (Register/Memory.hpp), an
                object shared with an ISR or another core through std::atomic / std::atomic_ref. `[rules]
                mmio_exempt = { R1 = ["<repo>/<file glob>", ...], ... }` names the files a rule does not apply to (the
                register layer's own one access). Not seen, left to review: a snapshot `auto s = readWhole(R);` masked
                later, `std::bit_cast` or `memcpy` aliasing, a volatile object declared as such.
  (e) cycle     files that include each other, directly or through others (a strongly connected group of the
                file-level include graph): neither can be read or compiled alone. An include resolves next to the
                including file, through a `repo` spelling to that file, or through a `layer` spelling (`chip/X.hpp`)
                to every scanned file of that layer whose path ends in the spelling (each chip's src/chip/X.hpp).

Comments are stripped before matching. There is no allow-list: a finding is NEW (exit 1) unless the table lists it
under `[keep.<check>]` (KEPT: a deliberate exception, its reason names the rule that makes it one). A keep entry
whose file or repository was scanned but whose finding is gone is STALE (exit 1). A top-level table the tool does
not know is a table error. `[rules] guarded_has_include = ["MACRO", ...]`: an
`#if defined(MACRO) && __has_include(H)` / `#elif ...` line is no has_include finding - MACRO, set by the build, is
the switch, and the `__has_include` only checks that the optional header is there; any other macro is a finding.

Repositories are found by name: `--repo NAME=PATH`, or `<root>/<NAME>` with `--root`; a table entry with `parent`
and `subdir` (a nested submodule) is looked for inside its parent's directory. A repository that is not found is
not scanned, and its keep entries are neither checked nor stale. Symlinked directories, nested checkouts
(a directory with its own .git) and the directories of other found repositories are not walked into. An overlay may
add `[default_repo] layer = ..., list = "<file below --root>", skip = [...]`: every other checkout the list names
(the first word of each line that is a directory below --root; `#` comments) is scanned as a repository whose files
are all in that layer.

Keys: `<repo>/<file> -> <spelling>` (include), `<repo>/<file>: <spelling>` (has_include), `<user repo> ->
<declaring repo>: <namespace>::<name>` (detail; found, and stale, only when both repositories were scanned),
`<repo>/<file>: R<n> <matched text, whitespace collapsed>` (mmio), `<repo>/<file> <-> <repo>/<file> [<-> ...]` (cycle,
the group's files sorted; found, and stale, only when all of them were scanned). An mmio entry whose key matches
more than once in its file is `key = ["reason", count]`: more hits than the count are NEW, fewer are STALE (lower
the count).

    layer_check.py --repo Kvasir_SDK=<path>                  a lone checkout (its nested repos included)
    layer_check.py --root <dir> [--overlay extra.toml]       every repository the table names, below <dir>
    options: --report (the component matrix, the layer cycles and every KEPT entry; exit 0 also with findings),
             --table <toml> (default tools/layers.toml), --overlay <toml> (merged over the table, repeatable), --json
             (the same data as JSON), --quiet (no matrix)
    tests: tests/layer_check/test_layer_check.py (fixture tree), ctest kvasir_test_layers (this repository)

Exit code: 0 no NEW and no STALE (or --report); 1 a NEW finding or a STALE entry; 2 a table error (an unknown
top-level table, an unknown layer, a path glob that matches no file of a scanned repository, a malformed or empty
entry).

A NEW finding is fixed - the include goes down, the name becomes public, the switch comes from the build - not
listed. A header that moves, or a new layer edge, changes the table in the same change.
"""

from __future__ import annotations

import argparse
import fnmatch
import json
import os
import re
import sys
from collections import defaultdict
from dataclasses import dataclass, field
from pathlib import Path

try:
    import tomllib
except ImportError:  # pragma: no cover
    sys.exit("layer_check.py needs Python >= 3.11 (tomllib)")

SOURCE_EXT = (".hpp", ".h", ".hh", ".hxx", ".cpp",
              ".cc", ".cxx", ".inc", ".ipp", ".tpp")
LOG_KINDS = ("log", "log_adapter")
CHECKS = ("include", "detail", "has_include", "mmio", "cycle")
TABLES = ("order", "layer", "repo", "spelling",
          "skip_dirs", "keep", "rules", "default_repo")


class TableError(Exception):
    pass


# --- globs ------------------------------------------------------------------

def glob_regex(pattern: str) -> re.Pattern:
    """`**` any path (also empty, `**/` zero or more directories), `*` and `?` within one path part, `{a,b}`."""
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


# --- source text --------------------------------------------------------------

def strip_source(text: str, blank_strings: bool) -> str:
    """Comments become spaces (newlines kept, so line numbers stay). With blank_strings the contents of string and
    char literals become spaces too (the quotes stay)."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
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
            body = text[i:end]
            out.append(re.sub(r"[^\n]", " ", body) if blank_strings else body)
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
            if blank_strings:
                out.append(
                    c + re.sub(r"[^\n]", " ", text[i + 1:j - 1]) + (text[j - 1] if j - 1 > i else ""))
            else:
                out.append(text[i:j])
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


INCLUDE = re.compile(r'^[ \t]*#[ \t]*include[ \t]*([<"])([^>"\n]+)[>"]', re.M)
# a line continuation may split it
HAS_INCLUDE = re.compile(r'__has_include[\s\\]*\(([^)]*)\)')
NS_OR_BRACE = re.compile(
    r"(?P<ns>\bnamespace\s+(?P<name>[A-Za-z_]\w*(?:\s*::\s*(?:inline\s+)?[A-Za-z_]\w*)*)?\s*\{)|(?P<open>\{)|(?P<close>\})")
DETAIL_USE = re.compile(
    r"(?<![\w:])(?P<chain>(?:::\s*)?(?:[A-Za-z_]\w*\s*::\s*)*?)(?P<d>detail|Detail)\s*::\s*(?:template\s+)?(?P<x>[A-Za-z_]\w*)")
GUARDED = re.compile(r'\s*#\s*(?:el)?if\s+defined\s*(?:\(\s*([A-Za-z_]\w*)\s*\)|\s([A-Za-z_]\w*))\s*&&\s*'
                     r'__has_include\s*\(\s*(?:<[^>\n]+>|"[^"\n]+")\s*\)\s*\Z')
MMIO_RULES = {
    "R1": re.compile(r"\b(?:reinterpret_cast|static_cast)\s*<[^;>]*\bvolatile\b[^;>]*\*\s*>"
                     r"|\(\s*(?:[\w:]+\s+)*?volatile(?:\s+[\w:]+)*\s*\*\s*\)"),
    "R2": re.compile(r"\breadWhole\s*\([^;]*?\)\s*[&|](?![&|=])|(?<![&|])[&|]\s*readWhole\s*\("
                     r"|\bwriteWhole\s*\([^,;]+,\s*(?:Kvasir::)?(?:Register::)?readWhole\b"),
    "R3": re.compile(r"\bRegister::Address<\s*(?:0[xX][0-9A-Fa-f']+|\d[\d']*)\b"),
    "R4": re.compile(r"\bRegister::(?:Address|Action)\s*<(?!\s*(?:0[xX][0-9A-Fa-f']+|\d[\d']*)\b)"),
}
TYPE_DECL = re.compile(r"^\s*(?:struct|class|union|enum(?:\s+class|\s+struct)?|concept|using)\s+"
                       r"(?:\[\[[^\]]*\]\]\s*)?(?:alignas\s*\([^)]*\)\s*)?([A-Za-z_]\w*)")
NOT_A_NAME = {"return", "else", "case", "co_return", "throw", "new", "delete", "sizeof", "decltype", "operator",
              "namespace", "noexcept", "requires", "if", "while", "for", "switch", "static_assert", "alignof"}


def line_of(text: str, pos: int) -> int:
    return text.count("\n", 0, pos) + 1


def balanced(s: str, i: int, open_: str, close: str) -> int:
    """The index after the bracket that closes the one at s[i]."""
    depth = 0
    while i < len(s):
        depth += 1 if s[i] == open_ else -1 if s[i] == close else 0
        i += 1
        if depth == 0:
            return i
    return i


def drop_template_heads(s: str) -> str:
    """Leading `template<...>` heads and the requires-clause after them (`requires(...)`, `requires C<T> && (...)`)."""
    while True:
        m = re.match(r"\s*template\s*<", s)
        if m:
            s = s[balanced(s, m.end() - 1, "<", ">"):]
            continue
        m = re.match(r"\s*requires\b", s)
        if not m:
            return s
        i = m.end()
        while True:   # a requires-clause: primaries joined by && / ||
            j = len(s) - len(s[i:].lstrip())
            if s.startswith("(", j):
                i = balanced(s, j, "(", ")")
            else:
                name = re.match(r"(?:::)?[A-Za-z_][\w:]*", s[j:])
                if not name:
                    break
                i = j + name.end()
                k = len(s) - len(s[i:].lstrip())
                if s.startswith("<", k):
                    i = balanced(s, k, "<", ">")
            k = len(s) - len(s[i:].lstrip())
            if not s.startswith(("&&", "||"), k):
                break
            i = k + 2
        s = s[i:]


def declared_names(piece: str) -> list[str]:
    """Names a piece of namespace-level text declares (types, aliases, concepts, functions, variables)."""
    names = []
    for stmt in piece.split(";"):
        s = drop_template_heads(re.sub(r"\[\[.*?\]\]", " ", stmt, flags=re.S))
        if re.match(r"\s*using\s+namespace\b", s):
            continue
        m = re.match(
            r"\s*using\s+((?:::\s*)?[A-Za-z_]\w*(?:\s*::\s*[A-Za-z_]\w*)+)\s*$", s)
        if m:  # a using-declaration names its last part here
            names.append(re.split(r"\s*::\s*", m.group(1))[-1])
            continue
        m = TYPE_DECL.match(s)
        if m:
            names.append(m.group(1))
            continue
        head = re.split(r"[(={\[]", s, maxsplit=1)[0]
        words = re.findall(r"[A-Za-z_]\w*", head)
        if len(words) >= 2 and words[-1] not in NOT_A_NAME and words[-2] != "operator":
            names.append(words[-1])
    return names


def detail_declarations(text: str) -> list[tuple[tuple[str, ...], str]]:
    """(namespace path, name) for everything declared directly in a namespace whose last part is detail/Detail."""
    out = []
    # a namespace frame holds its full path; another brace None
    stack: list[tuple[str, ...] | None] = []
    pos = 0

    def ns_path() -> tuple[str, ...]:
        for f in reversed(stack):
            if f is not None:
                return f
        return ()

    def top_is_detail() -> bool:
        return bool(stack) and stack[-1] is not None and stack[-1] and stack[-1][-1] in ("detail", "Detail")

    for m in NS_OR_BRACE.finditer(text):
        if top_is_detail():
            path = stack[-1]
            out += [(path, x) for x in declared_names(text[pos:m.start()])]
        if m.group("ns"):
            parts = tuple(p.strip().removeprefix("inline").strip()
                          for p in (m.group("name") or "").split("::") if p.strip())
            stack.append(ns_path() + parts)
        elif m.group("open"):
            stack.append(None)
        elif stack:
            stack.pop()
        pos = m.end()
    return out


# --- table ------------------------------------------------------------------

@dataclass
class Layer:
    name: str
    rank: int | None          # None: a log layer, beside the order
    kind: str                 # "ranked", "log", "log_adapter"
    log: bool
    has_include: bool
    # may include a log adapter (opt-in code: firmware)
    log_adapter: bool = False


@dataclass
class Repo:
    name: str
    paths: list[tuple[str, re.Pattern, str]]
    exclude: list[re.Pattern]
    parent: str | None = None
    subdir: str | None = None
    root: Path | None = None
    # repo-relative path -> layer
    files: dict[str, str] = field(default_factory=dict)

    def layer_of(self, relpath: str) -> str | None:
        if any(p.match(relpath) for p in self.exclude):
            return None
        for _, rx, layer in self.paths:
            if rx.match(relpath):
                return layer
        return None


def entries_of(data: dict) -> tuple[dict[str, dict[str, str]], dict[str, int]]:
    """[keep.<check>]: key -> a non-empty reason; an mmio entry may be ["reason", count] (count >= 2). Returns the
    reasons per check and the mmio counts (1 where none is given)."""
    tables = data.get("keep", {})
    unknown = set(tables) - set(CHECKS)
    if unknown:
        raise TableError(
            f"[keep.{sorted(unknown)[0]}]: the checks are {', '.join(CHECKS)}")
    out: dict[str, dict[str, str]] = {c: {} for c in CHECKS}
    counts: dict[str, int] = {}
    for check, entries in tables.items():
        for key, value in entries.items():
            reason, count = value, 1
            if check == "mmio" and isinstance(value, list):
                if len(value) != 2 or not isinstance(value[1], int) or isinstance(value[1], bool) or value[1] < 2:
                    raise TableError(
                        f"[keep.{check}] {key!r}: [\"reason\", count] with a count >= 2")
                reason, count = value
            if not isinstance(reason, str) or not reason.strip():
                raise TableError(f"[keep.{check}] {key!r}: needs a reason")
            out[check][key] = reason
            if check == "mmio":
                counts[key] = count
    return out, counts


def merge(base: dict, extra: dict) -> dict:
    out = dict(base)
    for k, v in extra.items():
        out[k] = merge(out[k], v) if isinstance(
            v, dict) and isinstance(out.get(k), dict) else v
    return out


def load_table(table: Path, overlays: list[Path]) -> dict:
    data = tomllib.loads(table.read_text())
    for o in overlays:
        data = merge(data, tomllib.loads(o.read_text()))
    return data


def build_layers(data: dict) -> dict[str, Layer]:
    layers = {}
    order = data.get("order", [])
    for name, spec in data.get("layer", {}).items():
        kind = spec.get("kind", "ranked")
        if kind == "ranked":
            same = spec.get("same_as", name)
            if same not in order:
                raise TableError(
                    f"layer {name}: not in `order` (nor same_as a layer in it)")
            rank = order.index(same)
        elif kind in LOG_KINDS:
            rank = None
        else:
            raise TableError(f"layer {name}: unknown kind {kind!r}")
        unknown = set(spec) - {"kind", "same_as", "log",
                               "has_include", "log_adapter"}
        if unknown:
            raise TableError(
                f"layer {name}: unknown key(s) {', '.join(sorted(unknown))}")
        layers[name] = Layer(name, rank, kind, bool(spec.get("log", True)), bool(spec.get("has_include", True)),
                             bool(spec.get("log_adapter", False)))
    for name in order:
        if name not in layers:
            raise TableError(
                f"order names {name!r}, which has no [layer.{name}]")
    return layers


def build_repos(data: dict, layers: dict[str, Layer]) -> dict[str, Repo]:
    repos = {}
    for name, spec in data.get("repo", {}).items():
        paths = []
        for glob, layer in spec.get("paths", {}).items():
            if layer not in layers:
                raise TableError(
                    f"repo {name}: path {glob!r} -> unknown layer {layer!r}")
            paths.append((glob, glob_regex(glob), layer))
        exclude = [glob_regex(g) for g in spec.get("exclude", [])]
        if ("parent" in spec) != ("subdir" in spec):
            raise TableError(f"repo {name}: `parent` and `subdir` go together")
        repos[name] = Repo(name, paths, exclude, spec.get(
            "parent"), spec.get("subdir"))
    return repos


@dataclass
class Spelling:
    prefix: str
    layer: str | None
    repo: str | None
    path: str


def build_spellings(data: dict, layers: dict[str, Layer], repos: dict[str, Repo]) -> list[Spelling]:
    out = []
    for e in data.get("spelling", []):
        if "prefix" not in e or ("layer" in e) == ("repo" in e):
            raise TableError(
                f"spelling {e}: needs `prefix` and one of `layer`, `repo`")
        if "layer" in e and e["layer"] not in layers:
            raise TableError(
                f"spelling {e['prefix']!r}: unknown layer {e['layer']!r}")
        if "repo" in e and e["repo"] not in repos:
            raise TableError(
                f"spelling {e['prefix']!r}: unknown repo {e['repo']!r}")
        out.append(Spelling(e["prefix"], e.get("layer"),
                   e.get("repo"), e.get("path", "")))
    return sorted(out, key=lambda s: -len(s.prefix))


# --- scanning ---------------------------------------------------------------

def locate(repos: dict[str, Repo], given: dict[str, Path], root: Path | None) -> None:
    def find(r: Repo, seen: set[str]) -> Path | None:
        if r.root is not None:
            return r.root
        if r.name in given:
            return given[r.name]
        if r.parent:
            if r.parent in seen or r.parent not in repos:
                return None
            p = find(repos[r.parent], seen | {r.name})
            return p / r.subdir if p is not None and (p / r.subdir).is_dir() else None
        if root is not None and (root / r.name).is_dir():
            return root / r.name
        return None

    for r in repos.values():
        r.root = find(r, set())
        if r.root is not None:
            r.root = Path(os.path.abspath(r.root))


def walk(repo: Repo, other_roots: set[str], skip_dirs: list[str]) -> list[str]:
    files = []
    for dp, dn, fn in os.walk(repo.root, followlinks=False):
        keep = []
        for d in dn:
            full = os.path.join(dp, d)
            if os.path.islink(full) or any(fnmatch.fnmatchcase(d, s) for s in skip_dirs):
                continue
            if os.path.realpath(full) in other_roots or os.path.exists(os.path.join(full, ".git")):
                continue
            keep.append(d)
        dn[:] = sorted(keep)
        for f in sorted(fn):
            if f.endswith(SOURCE_EXT) and not os.path.islink(os.path.join(dp, f)):
                files.append(os.path.relpath(os.path.join(
                    dp, f), repo.root).replace(os.sep, "/"))
    return files


@dataclass
class Finding:
    check: str
    key: str
    where: list[str]
    detail: str
    repo: str
    file: str | None = None


class Checker:
    def __init__(self, data: dict, given: dict[str, Path], root: Path | None):
        unknown = sorted(set(data) - set(TABLES))
        if unknown:
            raise TableError(f"unknown table(s) {', '.join(f'[{t}]' for t in unknown)}: the table has "
                             + ", ".join(TABLES))
        self.layers = build_layers(data)
        self.repos = build_repos(data, self.layers)
        self.spellings = build_spellings(data, self.layers, self.repos)
        self.skip_dirs = data.get(
            "skip_dirs", [".git", "build", "build_*", "_deps"])
        self.entries, self.counts = entries_of(data)
        rules = data.get("rules", {})
        known_rules = {"guarded_has_include", "mmio_exempt"}
        if set(rules) - known_rules:
            raise TableError(
                f"rules: unknown key(s) {', '.join(sorted(set(rules) - known_rules))}")
        exempt = rules.get("mmio_exempt", {})
        if not isinstance(exempt, dict) or set(exempt) - set(MMIO_RULES) or not all(
                isinstance(g, list) and all(isinstance(x, str) for x in g) for g in exempt.values()):
            raise TableError(
                f"rules.mmio_exempt: {{ R1 = [\"<repo>/<glob>\", ...], ... }} (rules {', '.join(MMIO_RULES)})")
        self.mmio_exempt = {
            r: [glob_regex(g) for g in exempt.get(r, [])] for r in MMIO_RULES}
        self.guards = rules.get("guarded_has_include", [])
        if not isinstance(self.guards, list) or not all(isinstance(g, str) and re.fullmatch(r"[A-Za-z_]\w*", g)
                                                        for g in self.guards):
            raise TableError(
                "rules.guarded_has_include: a list of macro names")
        for name in data.get("default_repo", {}):
            if name not in ("layer", "list", "skip"):
                raise TableError(f"default_repo.{name}: unknown key")
        self.default_repo = data.get("default_repo")
        self.root = root
        locate(self.repos, given, root)
        self.add_default_repos()
        self.errors: list[str] = []

    def add_default_repos(self) -> None:
        d = self.default_repo
        if not d or self.root is None:
            return
        if d.get("layer") not in self.layers:
            raise TableError(f"default_repo: unknown layer {d.get('layer')!r}")
        listing = self.root / d.get("list", "")
        if "list" not in d or not listing.is_file():
            raise TableError(
                f"default_repo.list: the file below --root that names the checkouts ({listing})")
        names = []
        for line in listing.read_text().splitlines():
            words = line.split()
            if words and not words[0].startswith("#") and words[0] not in names:
                names.append(words[0])
        taken = {os.path.realpath(r.root)
                 for r in self.repos.values() if r.root}
        for name in names:
            p = self.root / name
            if (name in self.repos or name in d.get("skip", []) or p.is_symlink() or not p.is_dir()
                    or os.path.realpath(p) in taken):
                continue
            self.repos[name] = Repo(
                name, [("**", glob_regex("**"), d["layer"])], [], root=p)

    # where an include goes
    def owner(self, real: str) -> tuple[Repo, str] | None:
        best = None
        for r in self.repos.values():
            if r.root is None:
                continue
            rr = os.path.realpath(r.root)
            if real.startswith(rr + "/") and (best is None or len(rr) > len(best[1])):
                best = (r, rr)
        if best is None:
            return None
        return best[0], os.path.relpath(real, best[1]).replace(os.sep, "/")

    def target_layer(self, src_dir: Path, quote: str, spelling: str) -> tuple[str, str] | None:
        """(component, layer) of an include target, or None for standard and third-party headers."""
        if quote == '"':
            cand = src_dir / spelling
            if cand.is_file():
                o = self.owner(os.path.realpath(cand))
                if o is None:
                    return None
                layer = o[0].layer_of(o[1])
                return (f"{o[0].name}:{layer}", layer) if layer else None
        for s in self.spellings:
            if spelling.startswith(s.prefix):
                if s.layer:
                    return (s.layer if self.layers[s.layer].kind in LOG_KINDS else f"*:{s.layer}", s.layer)
                r = self.repos[s.repo]
                layer = r.layer_of(s.path + spelling[len(s.prefix):])
                return (f"{r.name}:{layer}", layer) if layer else None
        return None

    def target_files(self, src_dir: Path, quote: str, spelling: str) -> list[str]:
        """`<repo>/<file>` of every scanned file an include may name: next to the including file, through a repo
        spelling, or - a layer spelling - each file of that layer whose path ends in the spelling."""
        if quote == '"':
            cand = src_dir / spelling
            if cand.is_file():
                o = self.owner(os.path.realpath(cand))
                return [f"{o[0].name}/{o[1]}"] if o is not None and o[1] in o[0].files else []
        for s in self.spellings:
            if spelling.startswith(s.prefix):
                if s.repo:
                    r = self.repos[s.repo]
                    f = s.path + spelling[len(s.prefix):]
                    return [f"{r.name}/{f}"] if r.root is not None and f in r.files else []
                rank = self.layers[s.layer].rank
                return [key for key, layer in self.by_suffix.get(spelling, [])
                        if layer == s.layer or (rank is not None and self.layers[layer].rank == rank)]
        return []

    def include_verdict(self, src: Layer, tgt: Layer) -> str | None:
        if src.kind == "log_adapter":
            return None
        if src.kind == "log":
            return None if tgt.kind in LOG_KINDS else "log library includes Kvasir"
        if tgt.kind == "log_adapter":
            return None if src.log_adapter else f"{src.name} -> log adapter"
        if tgt.kind == "log":
            return None if src.log else f"{src.name} (log = false) -> log"
        if tgt.rank > src.rank:
            return f"{src.name} -> {tgt.name} (upward)"
        return None

    def run(self) -> dict:
        found = [r for r in self.repos.values() if r.root is not None]
        roots = {os.path.realpath(r.root) for r in found}
        for r in found:
            for f in walk_cache(r, roots, self.skip_dirs):
                layer = r.layer_of(f)
                if layer:
                    r.files[f] = layer
            for glob, rx, _ in r.paths:
                if not any(rx.match(f) for f in walk_cache(r, roots, self.skip_dirs)):
                    self.errors.append(
                        f"repo {r.name}: path glob {glob!r} matches no file")
        # a layer spelling's targets: every file by each of its path suffixes (src/chip/X.hpp -> chip/X.hpp, X.hpp)
        self.by_suffix: dict[str, list[tuple[str, str]]] = defaultdict(list)
        for r in found:
            for f, layer in r.files.items():
                parts = f.split("/")
                for i in range(len(parts)):
                    self.by_suffix["/".join(parts[i:])
                                   ].append((f"{r.name}/{f}", layer))
        file_edges: dict[tuple[str, str], str] = {}

        findings: list[Finding] = []
        matrix: dict[tuple[str, str], int] = defaultdict(int)
        decls: dict[str, list[tuple[tuple[str, ...], str]]] = defaultdict(list)
        uses = []
        for r in found:
            for f, layer_name in r.files.items():
                path = r.root / f
                try:
                    raw = path.read_text(errors="replace")
                except OSError:
                    continue
                code = strip_source(raw, blank_strings=False)
                src = self.layers[layer_name]
                comp = f"{r.name}:{layer_name}"
                inc_hits: dict[str, list[str]] = defaultdict(list)
                inc_why = {}
                here = f"{r.name}/{f}"
                for m in INCLUDE.finditer(code):
                    for target in self.target_files(path.parent, m.group(1), m.group(2).strip()):
                        if target != here:
                            file_edges.setdefault(
                                (here, target), f"{here}:{line_of(code, m.start())}")
                    t = self.target_layer(
                        path.parent, m.group(1), m.group(2).strip())
                    if t is None:
                        continue
                    tcomp, tlayer = t
                    if tcomp != comp:
                        matrix[(comp, tcomp)] += 1
                    why = self.include_verdict(src, self.layers[tlayer])
                    if why:
                        key = f"{r.name}/{f} -> {m.group(2).strip()}"
                        inc_hits[key].append(
                            f"{r.name}/{f}:{line_of(code, m.start())}")
                        inc_why[key] = why
                findings += [Finding("include", k, v, inc_why[k], r.name, f)
                             for k, v in inc_hits.items()]
                if not src.has_include:
                    hi: dict[str, list[str]] = defaultdict(list)
                    for m in HAS_INCLUDE.finditer(code):
                        if self.guarded(code, m):
                            continue
                        hi[f"{r.name}/{f}: {re.sub(r'[\s\\]', '', m.group(1))}"].append(
                            f"{r.name}/{f}:{line_of(code, m.start())}")
                    findings += [Finding("has_include", k, v, f"__has_include in {layer_name}", r.name, f)
                                 for k, v in hi.items()]
                blank = strip_source(raw, blank_strings=True)
                mm: dict[str, list[str]] = defaultdict(list)
                for rule, rx in MMIO_RULES.items():
                    if any(g.match(f"{r.name}/{f}") for g in self.mmio_exempt[rule]):
                        continue
                    for m in rx.finditer(blank):
                        text = " ".join(m.group(0).split())
                        mm[f"{r.name}/{f}: {rule} {text}"].append(
                            f"{r.name}/{f}:{line_of(blank, m.start())}")
                findings += [Finding("mmio", k, v, "a raw access around the register layer", r.name, f)
                             for k, v in mm.items()]
                decls[r.name] += detail_declarations(blank)
                if src.kind not in LOG_KINDS:
                    for m in DETAIL_USE.finditer(blank):
                        if re.search(r"\bnamespace\s+$", blank[max(0, m.start() - 40):m.start()]):
                            continue
                        chain = tuple(p for p in re.split(
                            r"\s*::\s*", m.group("chain").strip()) if p) + (m.group("d"),)
                        uses.append((r.name, chain, m.group(
                            "x"), f"{r.name}/{f}:{line_of(blank, m.start())}"))

        by_name: dict[tuple[str, str],
                      list[tuple[str, tuple[str, ...]]]] = defaultdict(list)
        for repo, ds in decls.items():
            for p, x in ds:
                by_name[(p[-1], x)].append((repo, p))
        det_hits: dict[str, list[str]] = defaultdict(list)
        det_info = {}
        for repo, chain, x, where in uses:
            cands = [(rp, p) for rp, p in by_name.get(
                (chain[-1], x), []) if p[len(p) - len(chain):] == chain]
            owners = sorted({rp for rp, _ in cands})
            if not owners or repo in owners:
                continue
            path = next(p for rp, p in cands if rp == owners[0])
            key = f"{repo} -> {owners[0]}: {'::'.join(path)}::{x}"
            det_hits[key].append(where)
            det_info[key] = (repo, f"declared in {owners[0]}")
        findings += [Finding("detail", k, v, det_info[k][1], det_info[k][0])
                     for k, v in det_hits.items()]
        for group in cycles(set(file_edges)):
            members = set(group)
            where = sorted(w for (a, b), w in file_edges.items()
                           if a in members and b in members)
            findings.append(Finding("cycle", " <-> ".join(group), where, "files that include each other",
                                    group[0].split("/", 1)[0]))

        scanned_files = {f"{r.name}/{f}" for r in found for f in r.files}
        scanned_repos = {r.name for r in found}
        result = {"new": [], "kept": [], "stale": []}
        seen = set()
        for fd in sorted(findings, key=lambda x: (x.check, x.key)):
            seen.add((fd.check, fd.key))
            entry = {"check": fd.check, "key": fd.key,
                     "why": fd.detail, "where": fd.where}
            reason = self.entries[fd.check].get(fd.key)
            if reason is None:
                result["new"].append(entry)
                continue
            entry["reason"] = reason
            listed = self.counts.get(
                fd.key, 1) if fd.check == "mmio" else len(fd.where)
            if len(fd.where) > listed:
                entry["why"] += f"; {len(fd.where)} hits, the [keep.mmio] entry counts {listed}"
                result["new"].append(entry)
                continue
            result["kept"].append(entry)
            if len(fd.where) < listed:
                result["stale"].append({"check": fd.check, "key": fd.key, "reason": reason,
                                        "count": f"{len(fd.where)} hits, the entry counts {listed}"})
        for check, entries in self.entries.items():
            for key, reason in entries.items():
                if (check, key) in seen:
                    continue
                if check == "detail":
                    # "<user> -> <owner>: <name>": both must have been scanned to say the use is gone
                    applies = set(key.split(":", 1)[0].split(
                        " -> ")) <= scanned_repos
                elif check == "cycle":
                    applies = set(key.split(" <-> ")) <= scanned_files
                else:
                    applies = re.split(
                        r" -> |: ", key, maxsplit=1)[0] in scanned_files
                if applies:
                    result["stale"].append(
                        {"check": check, "key": key, "reason": reason})
        result["matrix"] = [{"from": a, "to": b, "count": n, "upward": self.upward(a, b)}
                            for (a, b), n in sorted(matrix.items())]
        # cycles between layers (a target named only by its spelling, "chip/...", names no repository): every pair
        # of layers that include each other, and the strongly connected groups
        layer_edges = {(a.split(":")[-1], b.split(":")[-1]) for (a, b) in matrix
                       if a.split(":")[-1] != b.split(":")[-1]}
        result["cycles"] = sorted([a, b] for (
            a, b) in layer_edges if a < b and (b, a) in layer_edges)
        result["cycle_groups"] = cycles(layer_edges)
        result["scanned"] = {r.name: len(r.files) for r in found}
        result["errors"] = self.errors
        return result

    def guarded(self, code: str, m: re.Match) -> bool:
        """`#if defined(MACRO) && __has_include(H)` (or #elif) with MACRO in rules.guarded_has_include."""
        if not self.guards:
            return False
        start = code.rfind("\n", 0, m.start()) + 1
        while start >= 2 and code[start - 2] == "\\":
            start = code.rfind("\n", 0, start - 1) + 1
        end = code.find("\n", m.end())
        while end > 0 and code[end - 1] == "\\":
            end = code.find("\n", end + 1)
        line = code[start:end if end >= 0 else len(code)].replace("\\\n", " ")
        g = GUARDED.match(line)
        return bool(g) and (g.group(1) or g.group(2)) in self.guards

    def upward(self, a: str, b: str) -> bool:
        la, lb = self.layers[a.split(":")[-1]], self.layers[b.split(":")[-1]]
        why = self.include_verdict(la, lb)
        return why is not None


_walk_cache: dict[str, list[str]] = {}


def walk_cache(r: Repo, roots: set[str], skip: list[str]) -> list[str]:
    key = str(r.root)
    if key not in _walk_cache:
        _walk_cache[key] = walk(r, roots - {os.path.realpath(r.root)}, skip)
    return _walk_cache[key]


def cycles(edges: set[tuple[str, str]]) -> list[list[str]]:
    """Strongly connected components with more than one member (Tarjan, iterative)."""
    graph: dict[str, list[str]] = defaultdict(list)
    for a, b in edges:
        graph[a].append(b)
        graph.setdefault(b, [])
    index: dict[str, int] = {}
    low: dict[str, int] = {}
    on, stack, out, counter = set(), [], [], [0]
    for start in sorted(graph):
        if start in index:
            continue
        work = [(start, iter(sorted(graph[start])))]
        index[start] = low[start] = counter[0]
        counter[0] += 1
        stack.append(start)
        on.add(start)
        while work:
            v, it = work[-1]
            for w in it:
                if w not in index:
                    index[w] = low[w] = counter[0]
                    counter[0] += 1
                    stack.append(w)
                    on.add(w)
                    work.append((w, iter(sorted(graph[w]))))
                    break
                if w in on:
                    low[v] = min(low[v], index[w])
            else:
                work.pop()
                if work:
                    low[work[-1][0]] = min(low[work[-1][0]], low[v])
                if low[v] == index[v]:
                    comp = []
                    while True:
                        w = stack.pop()
                        on.discard(w)
                        comp.append(w)
                        if w == v:
                            break
                    if len(comp) > 1:
                        out.append(sorted(comp))
    return sorted(out)


def print_report(res: dict, report: bool, quiet: bool) -> None:
    print("scanned: " +
          ", ".join(f"{k} ({v} files)" for k, v in sorted(res["scanned"].items())))
    if report and not quiet:
        print("\ncomponent matrix (includes between components; ^ = against the table):")
        for e in res["matrix"]:
            print(
                f"  {'^' if e['upward'] else ' '} {e['from']:<40} -> {e['to']:<40} {e['count']:5d}")
        print("\ncycles between layers (information only):")
        for c in res["cycles"]:
            print("  " + " <-> ".join(c))
        for g in res["cycle_groups"]:
            print("  strongly connected: " + ", ".join(g))
        if not res["cycles"] and not res["cycle_groups"]:
            print("  -")
    for label in (("kept",) if report else ()) + ("new", "stale"):
        print(f"\n{label.upper()}:")
        for e in res[label]:
            line = f"  [{e['check']}] {e['key']}"
            if "count" in e:
                line += f"   ({e['count']}: lower the count in [keep.{e['check']}])"
            elif label == "stale":
                line += f"   (the [keep.{e['check']}] entry has no finding left: remove it)"
            if "why" in e:
                line += f"   ({e['why']}; {', '.join(e['where'][:3])}{' ...' if len(e['where']) > 3 else ''})"
            if "reason" in e and label != "new":
                line += f"\n      reason: {e['reason']}"
            print(line)
        if not res[label]:
            print("  -")
    for err in res["errors"]:
        print(f"TABLE ERROR: {err}")
    cyc = ", ".join(" <-> ".join(c) for c in res["cycles"][:4]) or "none"
    print(f"\nsummary: {len(res['new'])} new, {len(res['stale'])} stale; {len(res['kept'])} kept; "
          f"{len(res['cycles'])} layer cycles "
          f"({cyc}{', ...' if len(res['cycles']) > 4 else ''})")


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--table", type=Path,
                    default=Path(__file__).with_name("layers.toml"))
    ap.add_argument("--overlay", type=Path, action="append", default=[])
    ap.add_argument("--repo", action="append", default=[], metavar="NAME=PATH")
    ap.add_argument("--root", type=Path)
    ap.add_argument("--report", action="store_true",
                    help="matrix, cycles and KEPT too; exit 0")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--quiet", action="store_true")
    a = ap.parse_args(argv)
    given = {}
    for spec in a.repo:
        name, sep, path = spec.partition("=")
        if not sep:
            ap.error(f"--repo {spec!r}: NAME=PATH")
        given[name] = Path(path)
    try:
        data = load_table(a.table, a.overlay)
        checker = Checker(data, given, a.root)
        unknown = [n for n in given if n not in checker.repos]
        if unknown:
            raise TableError(
                f"--repo names a repository the table does not know: {' '.join(unknown)}")
        res = checker.run()
    except (TableError, tomllib.TOMLDecodeError, OSError) as e:
        print(f"TABLE ERROR: {e}", file=sys.stderr)
        return 2
    if a.json:
        print(json.dumps(res, indent=1))
    else:
        print_report(res, a.report, a.quiet)
    if res["errors"]:
        return 2
    if not a.report and (res["new"] or res["stale"]):
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
