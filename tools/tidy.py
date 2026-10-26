#!/usr/bin/env python3
"""clang-tidy over configured clang build trees: the gate of the Kvasir repositories, locally and in CI.

One configuration, `../.clang-tidy` next to this tool, always passed with --config-file (no upward search, no
per-repository file). The runner picks one compile command per source, refreshes the generated headers, runs
clang-tidy in parallel within the machine's RAM, filters, deduplicates and attributes the findings, lints the NOLINT
markers, and decides pass or fail from the parsed output (never from clang-tidy's exit code).

    tidy.py --tree <build dir> [--tree ...] [--repo <checkout> ...] [mode] [options]

  --tree DIR          a configured clang tree with compile_commands.json (gcc trees are refused: clang-tidy would
                      take the host's libc++). Several trees: each (source, -mcpu) pair is tidied once.
  --repo DIR          the checkouts whose findings count and whose sources are main files (default: all - the
                      workspace run). Findings elsewhere are counted as "foreign" and do not fail the run.
  --nested            --repo also covers every nested checkout below it (Kvasir_SDK's uc_log, rtt, ...)
  --root DIR, --overlay TOML
                      the workspace run: the overlay's `[workspace] list = "<file below root>"` names the checkouts
                      whose NOLINT markers are linted and whose keep entries are checked; its [skip]/[must_fail]
                      entries are merged over tools/tidy_keep.toml.
modes (default: full)
  --changed [REV]     only translation units whose main file or include set holds a file changed since REV (default:
                      HEAD: the working tree with untracked files) in the checkouts of the run; a change to the config,
                      this tool, a keep table, the firmware list or the check lock means full
  --fix               apply the fix-its (exactly one --repo) through clang-apply-replacements' cleanup (the comma a
                      removed argument leaves), clang-format the touched files, tidy those units again
  --audit CHECK       run one check (on or off in the config) and report it; never fails on findings
  --check CHECK[,..]  run only these checks of the config (a gate on a subset; with --fix: one check per pass)
  --update-checks     rewrite tools/tidy_checks.txt from the live expansion of the config; print what changed
  --new-checks        print the checks the installed clang-tidy adds over tools/tidy_checks.txt (exit 1 if any)
  --self-test         the fixtures (one case runs the real clang-tidy)
  --export-fixes-dir DIR
                      keep every unit's export-fixes YAML as DIR/<n>.yaml and, once the unit is done, DIR/<n>.json
                      ({file, real, tree, rc, compile_failed, yaml}); no cache, nothing applied, any number of --repo (a
                      reader of the fix-its, e.g. a rename tool, reduces and deletes them as they come)
options
  --variant V         firmware variant whose command is taken (default debug: logs, self-tests, DCHECKs)
  --only GLOB[,..]    only sources matching (fnmatch on "<repo>/<path>" or the absolute path; `*` crosses `/`)
  --shard I/N         the I-th of N deterministic slices of the selected units (CI matrices)
  -j N                parallel clang-tidy processes (default: min(8, nproc, (MemAvailable - 4 GiB) / 1.75 GiB); CI:
                      nproc). A new process starts only while MemAvailable is at least 3 GiB.
  --no-build          do not build the trees' generated-header prerequisites first
  --wait S            wait this long for a ninja already running in a tree (default 600), then refuse
  --no-cache, --no-lock, --notes, --quiet, --format text|github

The shared build lock (~/.cache/kvasir_build.lock, when it exists): the runner takes it itself, per batch (the
prerequisite builds and the units up to a tree boundary after 2 minutes, at most 10 minutes), so other builds get in
between. Never wrap the runner in `flock` on the same file
(it would wait for itself); a caller that already holds the lock sets KVASIR_BUILD_LOCK_HELD=1. CI: --no-lock.

What is dropped (counted on the summary): sources of Kvasir_SDK/lib and _deps, assembler, keep-table paths
(tools/tidy_keep.toml [skip] and [must_fail]); findings whose primary location matches the config's
ExcludeHeaderFilterRegex (a note of an excluded header can make clang-tidy print it anyway), lies in a build tree or a
[skip] path; duplicates (same real file, line, column and check, across units and trees). Owner of a file: the
innermost directory holding a `.git`, after resolving symlinks.

Generated headers: on a Ninja tree the runner first builds the phonies `cmake_object_order_depends_target_<target>` of
the selected units (named targets, exactly the order-only prerequisites, no object), after checking that no ninja runs
in that tree. Makefiles trees (the CI firmware jobs) are tidied after their build.

Per-unit cache: <tree>/.tidy/cache, keyed by the clang-tidy version, the config, this tool, the lists, the unit's
command and the content of every file of its include set (ninja deps, else the .d depfile, else a -M run).

NOLINT markers in the scanned checkouts: allowed are `// NOLINTNEXTLINE(<check>[,<check>]) // <reason>`,
`// NOLINTBEGIN(<check>) // <reason>` ... `// NOLINTEND(<check>)`, and inside a macro `/* NOLINTNEXTLINE(<check>)
<reason> */`. A bare or same-line NOLINT, a glob, a name that is no enabled check or a missing reason is a
`nolint-format` finding; more than 3 markers naming one check is a `nolint-budget` finding (switch the check off or
tune it in the config instead) - counted over every scanned checkout together in a run without --repo (the workspace
run), per checkout in a --repo run.

Exit code: 0 clean; 1 findings (a diagnostic, a NOLINT finding, a STALE keep entry, an alias enabled twice);
2 usage or configuration (no database, a gcc tree, --verify-config failed, a busy tree, a tool missing);
3 could not check (clang-tidy crashed, a compile error, a prerequisite build failed).
"""

from __future__ import annotations

import argparse
import collections
import concurrent.futures as cf
import fcntl
import fnmatch
import functools
import hashlib
import json
import os
import re
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path

try:
    import tomllib
except ImportError:  # pragma: no cover
    sys.exit("tidy.py needs Python >= 3.11 (tomllib)")

HERE = Path(__file__).resolve().parent
SDK_DIR = HERE.parent
DEFAULT_CONFIG = SDK_DIR / '.clang-tidy'
DEFAULT_KEEP = HERE / 'tidy_keep.toml'
FIRMWARE_CHECKS_FILE = HERE / 'tidy_firmware_checks.txt'
CHECKS_LOCK_FILE = HERE / 'tidy_checks.txt'

GIB = 1 << 30
MEM_RESERVE = 4 * GIB          # left to everything else on the machine
# a firmware unit's clang-tidy peaks at 0.7-1.7 GB
MEM_PER_UNIT = int(1.75 * GIB)
MEM_ADMIT = 3 * GIB            # no new process below this MemAvailable
MAX_JOBS = 8
# the build lock is released for other builds at least this often
BATCH_SECONDS = 600
BATCH_MIN_SECONDS = 120        # ... and at a tree boundary once held this long
ANNOTATION_LIMIT = 10          # GitHub shows at most 10 error annotations per step
SUMMARY_LIMIT = 500
NOLINT_BUDGET = 3
CXX_SUFFIXES = {'.cpp', '.cc', '.cxx', '.c++', '.c',
                '.hpp', '.hh', '.hxx', '.h', '.ipp', '.tpp', '.inl'}
ASM_SUFFIXES = {'.s', '.S', '.asm'}
VARIANT_SUFFIXES = ('debug', 'release', 'release_log', 'sanitize', 'coverage')
LIB_RE = re.compile(r'/lib/(libc|libcxx|compiler-rt)/')
CRASH_SIGNALS = {signal.SIGSEGV, signal.SIGBUS}


class Fatal(Exception):
    """Stops the run with an exit code (2 usage/config, 3 could not check)."""

    def __init__(self, code, message):
        super().__init__(message)
        self.code = code


def in_ci():
    return os.environ.get('CI', '') in ('true', '1') or bool(os.environ.get('GITHUB_ACTIONS'))


def eprint(*a):
    print(*a, file=sys.stderr, flush=True)


# ---------------------------------------------------------------------------------------------------------------------
# globs, lists, keep tables

def glob_to_re(pattern):
    """`*` within a path part, `**` across parts (`**/` also matches no part), `?`, `{a,b}`."""
    out, i = '', 0
    while i < len(pattern):
        if pattern.startswith('**/', i):
            out += '(?:[^/]*/)*'
            i += 3
        elif pattern.startswith('**', i):
            out += '.*'
            i += 2
        elif pattern[i] == '*':
            out += '[^/]*'
            i += 1
        elif pattern[i] == '?':
            out += '[^/]'
            i += 1
        elif pattern[i] == '{' and '}' in pattern[i:]:
            j = pattern.index('}', i)
            out += '(?:' + '|'.join(re.escape(a)
                                    for a in pattern[i + 1:j].split(',')) + ')'
            i = j + 1
        else:
            out += re.escape(pattern[i])
            i += 1
    return re.compile(out + r'\Z')


def read_list(path):
    """Names from a one-per-line file; `#` starts a comment."""
    if not Path(path).is_file():
        return []
    names = []
    for line in Path(path).read_text().splitlines():
        line = line.split('#', 1)[0].strip()
        if line:
            names.append(line)
    return names


@dataclass
class KeepEntry:
    key: str
    kind: str  # skip | must_fail
    reason: str
    regex: re.Pattern
    origin: str

    @property
    def repo(self):
        first = self.key.split('/', 1)[0]
        return None if any(c in first for c in '*?{[') else first


def load_keep(paths):
    """The [skip] and [must_fail] entries of the tables, and the [workspace] table of the last one that has it."""
    entries, workspace = [], None
    for path in paths:
        try:
            data = tomllib.loads(Path(path).read_text())
        except (OSError, tomllib.TOMLDecodeError) as e:
            raise Fatal(2, f'keep table {path}: {e}')
        for table, rows in data.items():
            if table == 'workspace':
                workspace = rows
                continue
            if table not in ('skip', 'must_fail'):
                raise Fatal(
                    2, f'keep table {path}: unknown table [{table}] (known: skip, must_fail, workspace)')
            for key, reason in rows.items():
                if not isinstance(reason, str) or not reason.strip():
                    raise Fatal(
                        2, f'keep table {path}: "{key}" needs a reason')
                entries.append(KeepEntry(key, table, reason,
                               glob_to_re(key), str(path)))
    return entries, workspace


def keep_match(entries, display, kinds=('skip', 'must_fail')):
    for e in entries:
        if e.kind in kinds and e.regex.match(display):
            return e
    return None


# ---------------------------------------------------------------------------------------------------------------------
# ownership

class Owners:
    """The checkout owning a file: the innermost directory with a `.git` (directory or submodule file), realpath."""

    def __init__(self, roots=()):
        self.roots = {os.path.realpath(r) for r in roots}
        self.cache = {}

    def owner_of_dir(self, d):
        chain = []
        found = None
        while True:
            if d in self.cache:
                found = self.cache[d]
                break
            chain.append(d)
            if d in self.roots or os.path.exists(os.path.join(d, '.git')):
                found = d
                break
            parent = os.path.dirname(d)
            if parent == d:
                break
            d = parent
        for c in chain:
            self.cache[c] = found
        return found

    def owner(self, real):
        return self.owner_of_dir(os.path.dirname(real))

    def display(self, real):
        o = self.owner(real)
        if o is None:
            return real
        return os.path.basename(o) + '/' + os.path.relpath(real, o)


# ---------------------------------------------------------------------------------------------------------------------
# compile databases

class Entry:
    """One compile command. Cheap to make: the argument split and the realpath are computed when first used, so the
    tens of thousands of runtime-library and assembler commands of a firmware tree cost only their filters."""

    def __init__(self, directory, args, file, output, real, target, cpu, firmware, raw):
        self.directory, self.file, self.output, self.target = directory, file, output, target
        self.cpu, self.firmware, self.raw = cpu, firmware, raw
        if args is not None:
            self.__dict__['args'] = args
        if real is not None:
            self.__dict__['real'] = real

    @functools.cached_property
    def args(self):
        return self.raw['arguments'] if 'arguments' in self.raw else shlex.split(self.raw['command'])

    @functools.cached_property
    def real(self):
        return os.path.realpath(self.file)


@dataclass
class Tree:
    path: str
    real: str
    generator: str  # ninja | make | ''
    entries: list
    db_dir: str = ''


TARGET_RE = re.compile(
    r'(?:^|\s)(?:-mcpu=(?P<mcpu>\S+)|--target=(?P<t1>\S+)|-target\s+(?P<t2>\S+))')


def target_info(command):
    """(cpu key, firmware?) from a command line: -mcpu, else --target / -target, else 'host'."""
    mcpu = target = None
    for m in TARGET_RE.finditer(command):
        mcpu = m.group('mcpu') or mcpu
        target = m.group('t1') or m.group('t2') or target
    firmware = (mcpu or '').startswith(
        'cortex-m') or (target or '').startswith(('thumb', 'arm'))
    return (mcpu or target or 'host'), firmware


def make_entry(raw):
    directory = raw['directory']
    command = shlex.join(
        raw['arguments']) if 'arguments' in raw else raw['command']
    file = raw['file'] if os.path.isabs(
        raw['file']) else os.path.join(directory, raw['file'])
    output = raw.get('output', '')
    if output and not os.path.isabs(output):
        output = os.path.join(directory, output)
    m = re.search(r'CMakeFiles/([^/]+)\.dir/', output)
    cpu, firmware = target_info(command)
    return Entry(directory, None, os.path.normpath(file), os.path.normpath(output) if output else '', None,
                 m.group(1) if m else '', cpu, firmware, raw)


def compiler_id(tree_path, entries):
    for f in sorted(Path(tree_path).glob('CMakeFiles/*/CMakeCXXCompiler.cmake')):
        m = re.search(r'set\(CMAKE_CXX_COMPILER_ID "(\w*)"\)',
                      f.read_text(errors='replace'))
        if m:
            return m.group(1)
    for e in entries:
        exe = os.path.basename((e.raw.get('command') or ' '.join(
            e.raw.get('arguments', []))).split(' ', 1)[0])
        if 'clang' in exe:
            return 'Clang'
        if 'g++' in exe or 'gcc' in exe:
            return 'GNU'
    return ''


def scanned_entries(tree_path, known_outputs):
    """Units a Ninja tree compiles through C++ module-scan rules, which CMake leaves out of compile_commands.json."""
    rules_file = Path(tree_path) / 'CMakeFiles' / 'rules.ninja'
    if not rules_file.is_file():
        return []
    rules = re.findall(r'^rule (CXX_COMPILER__\S+_scanned_\S*)$',
                       rules_file.read_text(errors='replace'), re.M)
    if not rules:
        return []
    p = subprocess.run(['ninja', '-C', tree_path, '-t',
                       'compdb', *rules], capture_output=True, text=True)
    if p.returncode != 0:
        return []
    out = []
    for raw in json.loads(p.stdout or '[]'):
        if not raw.get('command'):
            continue
        args = [a for a in shlex.split(raw['command']) if not (
            a.startswith('@') and a.endswith('.modmap'))]
        raw = dict(raw, command=shlex.join(args))
        e = make_entry(raw)
        if e.output not in known_outputs:
            out.append(e)
    return out


def load_tree(path):
    path = os.path.abspath(path)
    db = os.path.join(path, 'compile_commands.json')
    if not os.path.isfile(db):
        raise Fatal(
            2, f'{path}: no compile_commands.json - configure it with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON')
    try:
        raws = json.loads(Path(db).read_text())
    except (OSError, ValueError) as e:
        raise Fatal(2, f'{db}: {e}')
    entries = [make_entry(r) for r in raws]
    cid = compiler_id(path, entries)
    if cid and cid != 'Clang':
        raise Fatal(2, f'{path}: a {cid} tree - gcc trees cannot be tidied (clang-tidy would take the host libc++); '
                    'give the clang tree')
    generator = 'ninja' if os.path.isfile(os.path.join(path, 'build.ninja')) else \
        'make' if os.path.isfile(os.path.join(path, 'Makefile')) else ''
    if generator == 'ninja':
        entries += scanned_entries(path, {e.output for e in entries})
    return Tree(path, os.path.realpath(path), generator, entries)


# ---------------------------------------------------------------------------------------------------------------------
# selecting the units

@dataclass
class Unit:
    tree: Tree
    entry: Entry
    header_alone: bool
    display: str
    includes: list | None = None   # realpaths, None = unknown
    cache_id: str = ''
    checks_arg: str = ''

    @property
    def real(self):
        return self.entry.real

    @property
    def key(self):
        return (self.entry.real, self.entry.cpu)


def variant_rank(entry, variant):
    if not entry.firmware:
        # the SDK's self-contained headers come twice: on the register mock and on the volatile (real) path
        return 0 if 'volatile' in entry.target else 1
    if entry.target == variant or entry.target.endswith('_' + variant):
        return 0
    if not any(entry.target == v or entry.target.endswith('_' + v) for v in VARIANT_SUFFIXES):
        return 1
    return 2


def define_count(entry):
    """The -D flags of a command: a compile-fail case of a test is the same source built once more with one define
    more (`kvasir_devices_compile_fail`, `#ifdef MUST_FAIL`), so among equally ranked commands the fewest wins."""
    return sum(1 for a in entry.args if a.startswith('-D'))


def is_excluded(path, exclude_re):
    return bool(exclude_re and exclude_re.search(path))


def select_units(trees, variant, keep, owners, repo_set, only, exclude_re, drops):
    """One entry per (source, cpu) over all trees; the rest is counted in drops."""
    candidates = collections.defaultdict(list)
    tree_reals = [t.real for t in trees]
    for order, tree in enumerate(trees):
        for e in tree.entries:
            suffix = os.path.splitext(e.file)[1]
            if suffix in ASM_SUFFIXES:
                drops['assembler source'] += 1
                continue
            if suffix not in CXX_SUFFIXES:
                drops['other source'] += 1
                continue
            if LIB_RE.search(e.file) or '/_deps/' in e.file or LIB_RE.search(e.real) or '/_deps/' in e.real:
                drops['runtime library or fetched source'] += 1
                continue
            if is_excluded(e.file, exclude_re) or is_excluded(e.real, exclude_re):
                drops['excluded source'] += 1
                continue
            in_tree = any(e.real.startswith(t + os.sep) for t in tree_reals)
            display = owners.display(e.real)
            if keep_match(keep, display):
                drops['keep table source'] += 1
                continue
            if repo_set is not None and owners.owner(e.real) not in repo_set:
                drops['source outside --repo'] += 1
                continue
            if only and not any(fnmatch.fnmatchcase(display, g) or fnmatch.fnmatchcase(e.real, g) for g in only):
                drops['--only'] += 1
                continue
            candidates[(e.real, e.cpu)].append(
                (variant_rank(e, variant), order, e.output, tree, e, in_tree, display))
    units = []
    for key in sorted(candidates):
        rows = candidates[key]
        if len(rows) > 1:
            rows = sorted(rows, key=lambda r: (
                r[0], define_count(r[4]), r[1], r[2]))
        _, _, _, tree, e, in_tree, display = rows[0]
        drops['duplicate command'] += len(rows) - 1
        units.append(Unit(tree, e, in_tree, display))
    return units


def shard(units, spec):
    if not spec:
        return units
    m = re.fullmatch(r'(\d+)/(\d+)', spec)
    if not m or not 1 <= int(m.group(1)) <= int(m.group(2)):
        raise Fatal(2, f'--shard {spec}: want I/N with 1 <= I <= N')
    i, n = int(m.group(1)), int(m.group(2))
    ordered = sorted(units, key=lambda u: u.key)
    return [u for k, u in enumerate(ordered) if k % n == i - 1]


def write_db(tree, units):
    """One entry per selected unit of the tree, in a database of this run (two runs never share one)."""
    d = tempfile.mkdtemp(
        prefix=f'db-{os.getpid()}-', dir=ensure_dir(os.path.join(tree.real, '.tidy')))
    with open(os.path.join(d, 'compile_commands.json'), 'w') as f:
        json.dump([u.entry.raw for u in units if u.tree is tree], f, indent=1)
    tree.db_dir = d


def ensure_dir(d):
    os.makedirs(d, exist_ok=True)
    return d


# ---------------------------------------------------------------------------------------------------------------------
# include sets

def parse_ninja_deps(text, base):
    """`ninja -t deps` output -> {output (absolute): [deps (realpath)] or None when ninja has none}."""
    result, current = {}, None
    for line in text.splitlines():
        m = re.match(
            r'^(\S.*?): #deps \d+, deps mtime \d+ \((?:VALID|STALE)\)$', line)
        if m:
            current = os.path.normpath(os.path.join(base, m.group(1)))
            result[current] = []
            continue
        m = re.match(r'^(\S.*?): deps not found$', line)
        if m:
            result[os.path.normpath(os.path.join(base, m.group(1)))] = None
            current = None
            continue
        if line.startswith('    ') and current is not None:
            result[current].append(os.path.realpath(
                os.path.join(base, line.strip())))
        elif not line.strip():
            current = None
    return result


def parse_make_deps(text, base):
    """A make-style dependency file (`-MD` / `-M`): the prerequisites, realpath."""
    text = text.replace('\\\r\n', ' ').replace('\\\n', ' ')
    deps = []
    for rule in re.split(r'(?<!\\)\n', text):
        if ':' not in rule:
            continue
        # the target ends at the first ': ' (a drive letter or an escaped colon does not end it)
        m = re.search(r'(?<!\\):(?:\s|$)', rule)
        if not m:
            continue
        for tok in re.findall(r'(?:\\.|[^\s\\])+', rule[m.end():]):
            tok = re.sub(r'\\(.)', r'\1', tok).replace('$$', '$')
            deps.append(os.path.realpath(os.path.join(base, tok)))
    return list(dict.fromkeys(deps))


def ninja_deps(tree, units):
    if tree.generator != 'ninja':
        return {}
    outs = [os.path.relpath(u.entry.output, tree.path)
            for u in units if u.tree is tree and u.entry.output]
    result = {}
    for i in range(0, len(outs), 400):
        p = subprocess.run(['ninja', '-C', tree.path, '-t', 'deps',
                           *outs[i:i + 400]], capture_output=True, text=True)
        if p.returncode == 0:
            result.update(parse_ninja_deps(p.stdout, tree.path))
    return result


def depfile_deps(entry):
    if not entry.output:
        return None
    d = entry.output + '.d'
    if not os.path.isfile(d):
        return None
    try:
        return parse_make_deps(Path(d).read_text(errors='replace'), entry.directory)
    except OSError:
        return None


def preprocess_deps(entry):
    """One -M run of the unit's command (no object written)."""
    args, skip = [], False
    drop_with_value = {'-o', '-MF', '-MT', '-MQ', '--serialize-diagnostics'}
    for a in entry.args:
        if skip:
            skip = False
            continue
        if a in drop_with_value:
            skip = True
            continue
        if a in ('-c', '-MD', '-MMD') or a.startswith(('-MF', '-MT', '-MQ')) and len(a) > 3:
            continue
        args.append(a)
    p = subprocess.run(args + ['-M', '-MF', '-'],
                       cwd=entry.directory, capture_output=True, text=True)
    if p.returncode != 0:
        return None
    return parse_make_deps(p.stdout, entry.directory)


# ---------------------------------------------------------------------------------------------------------------------
# cache

class Hasher:
    def __init__(self):
        self.lock = threading.Lock()
        self.files = {}

    def file(self, path):
        with self.lock:
            if path in self.files:
                return self.files[path]
        try:
            h = hashlib.sha256(Path(path).read_bytes()).hexdigest()
        except OSError:
            h = 'missing'
        with self.lock:
            self.files[path] = h
        return h

    def digest(self, paths):
        h = hashlib.sha256()
        for p in sorted(set(paths)):
            h.update(p.encode() + b'\0' + self.file(p).encode() + b'\n')
        return h.hexdigest()


def unit_identity(fingerprint, unit):
    e = unit.entry
    command = e.raw.get('arguments') or e.raw.get('command')
    return hashlib.sha256(json.dumps([fingerprint, e.directory, command, e.file, unit.checks_arg]).encode()).hexdigest()


def cache_path(unit):
    return os.path.join(unit.tree.real, '.tidy', 'cache', unit.cache_id + '.json')


def cache_load(unit, hasher):
    """The cached output when every file of the stored include set is unchanged; sets unit.includes either way."""
    try:
        data = json.loads(Path(cache_path(unit)).read_text())
    except (OSError, ValueError):
        return None
    if unit.includes is None:
        unit.includes = data.get('includes')
    if data.get('digest') != hasher.digest([unit.real] + data.get('includes', [])):
        return None
    return data.get('output')


def cache_store(unit, hasher, output):
    if unit.includes is None:
        return
    p = cache_path(unit)
    ensure_dir(os.path.dirname(p))
    tmp = p + f'.{os.getpid()}.tmp'
    Path(tmp).write_text(json.dumps({'includes': unit.includes, 'output': output,
                                     'digest': hasher.digest([unit.real] + unit.includes)}))
    os.replace(tmp, p)


# ---------------------------------------------------------------------------------------------------------------------
# clang-tidy output

DIAG_RE = re.compile(r'^(?P<file>.+?):(?P<line>\d+):(?P<col>\d+): (?P<sev>warning|error|note): (?P<msg>.*?)'
                     r'(?: \[(?P<checks>[A-Za-z0-9_.,+-]+)\])?$')


@dataclass
class Finding:
    file: str          # as spelled
    real: str
    line: int
    col: int
    check: str
    message: str
    checks: tuple = ()
    notes: list = field(default_factory=list)
    display: str = ''
    owner: str | None = None


def parse_output(text, base='/'):
    """Diagnostics with their notes. A check tag ends the line; `-warnings-as-errors` entries are dropped."""
    findings, current = [], None
    for line in text.splitlines():
        m = DIAG_RE.match(line)
        if not m:
            continue
        path = m.group('file')
        if not os.path.isabs(path):
            path = os.path.normpath(os.path.join(base, path))
        if m.group('sev') == 'note':
            if current is not None:
                current.notes.append(
                    (path, int(m.group('line')), int(m.group('col')), m.group('msg')))
            continue
        tags = tuple(t for t in (m.group('checks') or '').split(
            ',') if t and not t.startswith('-'))
        if not tags:
            # a diagnostic without a check tag: the driver or the frontend
            tags = ('clang-diagnostic-error',
                    ) if m.group('sev') == 'error' else ('clang-diagnostic-warning',)
        current = Finding(path, os.path.realpath(path), int(m.group('line')), int(m.group('col')), tags[0],
                          m.group('msg'), tags)
        findings.append(current)
    return findings


def compile_failed(text, findings):
    return any(f.check == 'clang-diagnostic-error' for f in findings) or 'Error while processing' in text


# ---------------------------------------------------------------------------------------------------------------------
# export-fixes YAML (the subset clang-tidy writes) and applying replacements

def _yaml_scalar(lines, i, rest):
    """A scalar starting at `rest` on line i; returns (value, next line index). Handles plain, '...' and "..."."""
    rest = rest.strip()
    if not rest or rest[0] not in '\'"':
        return rest, i + 1
    q = rest[0]
    buf, line_no = rest[1:], i
    parts = []
    while True:
        j = 0
        out = ''
        while j < len(buf):
            c = buf[j]
            if q == "'" and c == "'":
                if j + 1 < len(buf) and buf[j + 1] == "'":
                    out += "'"
                    j += 2
                    continue
                parts.append(out)
                return _fold(parts), line_no + 1
            if q == '"' and c == '\\' and j + 1 < len(buf):
                n = buf[j + 1]
                esc = {'n': '\n', 't': '\t', 'r': '\r', '0': '\0',
                       '"': '"', '\\': '\\', '/': '/', ' ': ' '}
                if n in esc:
                    out += esc[n]
                    j += 2
                    continue
                if n in 'xuU':
                    width = {'x': 2, 'u': 4, 'U': 8}[n]
                    out += chr(int(buf[j + 2:j + 2 + width], 16))
                    j += 2 + width
                    continue
                out += n
                j += 2
                continue
            if q == '"' and c == '"':
                parts.append(out)
                return _fold(parts), line_no + 1
            out += c
            j += 1
        parts.append(out)
        line_no += 1
        if line_no >= len(lines):
            return _fold(parts), line_no
        buf = lines[line_no].lstrip()


def _fold(parts):
    """YAML flow folding: a line break between non-empty lines is a space, each empty line a newline."""
    out = parts[0].rstrip(' ') if len(parts) > 1 else parts[0]
    newlines = 0
    for k in range(1, len(parts)):
        last = k == len(parts) - 1
        s = parts[k].lstrip(' ') if last else parts[k].strip(' ')
        if s == '' and not last:
            newlines += 1
            continue
        out += '\n' * newlines if newlines else ' '
        newlines = 0
        out += s
    return out


def parse_fixes_yaml(text):
    """[{'check', 'file', 'offset', 'replacements': [(file, offset, length, text)]}] of the main diagnostics."""
    lines = text.splitlines()
    diags, cur, in_notes, rep = [], None, False, None
    i = 0
    while i < len(lines):
        line = lines[i]
        s = line.strip()
        indent = len(line) - len(line.lstrip(' '))
        m = re.match(r'^(- )?([A-Za-z]+):\s?(.*)$', s)
        if not m:
            i += 1
            continue
        dash, key, rest = m.group(1), m.group(2), m.group(3)
        if rest.strip()[:1] in ('\'', '"') and key not in ('FilePath', 'Offset', 'Length', 'ReplacementText',
                                                           'FileOffset', 'DiagnosticName'):
            # a quoted scalar we do not need (Message, MainSourceFile, BuildDirectory) may span lines: skip all of it
            _, i = _yaml_scalar(lines, i, rest)
            continue
        if key == 'DiagnosticName':
            name, i = _yaml_scalar(lines, i, rest)
            cur = {'check': name, 'file': '', 'offset': 0, 'replacements': []}
            diags.append(cur)
            in_notes = False
            rep = None
            continue
        if cur is None:
            i += 1
            continue
        if key == 'Notes':
            in_notes = True
            i += 1
            continue
        if indent <= 4 and key in ('Level', 'BuildDirectory', 'Ranges'):
            in_notes = key == 'Ranges' and in_notes
        if key in ('FilePath', 'Offset', 'Length', 'ReplacementText', 'FileOffset'):
            value, i = _yaml_scalar(lines, i, rest)
            if in_notes:
                continue
            if dash and key == 'FilePath' and indent >= 8:
                rep = {'file': value}
                continue
            if rep is not None and indent >= 10:
                rep[key] = value
                if key == 'ReplacementText':
                    cur['replacements'].append(
                        (rep['file'], int(rep['Offset']), int(rep['Length']), value))
                    rep = None
                continue
            if key == 'FilePath' and indent == 6:
                cur['file'] = value
            elif key == 'FileOffset' and indent == 6:
                cur['offset'] = int(value)
            continue
        i += 1
    return diags


def _yaml_quote(text):
    """A double-quoted YAML scalar that LLVM's YAML parser reads back byte for byte."""
    out = []
    for ch in text:
        if ch in '"\\':
            out.append('\\' + ch)
        elif ch == '\n':
            out.append('\\n')
        elif ord(ch) < 0x20 or ord(ch) == 0x7f:
            out.append(f'\\x{ord(ch):02x}')
        else:
            out.append(ch)
    return '"' + ''.join(out) + '"'


def replacements_yaml(chosen_per_file):
    """{path: [(offset, length, text, check)]} -> one export-fixes document, one diagnostic per file."""
    lines = ['---', "MainSourceFile: ''", 'Diagnostics:']
    for path, chosen in sorted(chosen_per_file.items()):
        if not chosen:
            continue
        q = _yaml_quote(path)
        lines += ['  - DiagnosticName: tidy-fix', '    DiagnosticMessage:', "      Message: ''",
                  f'      FilePath: {q}', f'      FileOffset: {chosen[0][0]}', '      Replacements:']
        for off, length, text, _ in chosen:
            lines += [f'        - FilePath: {q}', f'          Offset: {off}', f'          Length: {length}',
                      f'          ReplacementText: {_yaml_quote(text)}']
        lines += ['    Level: Warning']
    return '\n'.join(lines + ['...']) + '\n'


def apply_replacements(per_file, tool='clang-apply-replacements'):
    """{realpath: [(offset, length, text, check)]} -> (applied {path: count}, conflicts [(path, check)]).

    The non-overlapping replacements are written as one export-fixes file and applied by clang-apply-replacements with
    its cleanup on and formatting off (`--format --style={DisableFormat: true}`): the cleanup removes the comma that a
    deleted argument leaves (modernize-use-ranges deletes `v.end()` from `copy(v.begin(), v.end(), at)` and relies on
    it), as clang-tidy's own --fix does."""
    applied, conflicts, chosen_of = {}, [], {}
    for path, reps in per_file.items():
        uniq = sorted(set(reps), key=lambda r: (r[0], r[1], r[2]))
        chosen, end = [], -1
        for r in uniq:
            if chosen and (r[0], r[1], r[2]) == chosen[-1][:3]:
                continue
            if r[0] < end or (r[0] == end and r[1] == 0 and chosen and chosen[-1][1] == 0 and chosen[-1][0] == r[0]):
                conflicts.append((path, r[3]))
                continue
            chosen.append(r)
            end = max(end, r[0] + r[1])
        chosen_of[path] = chosen
        applied[path] = len(chosen)
    if any(chosen_of.values()):
        with tempfile.TemporaryDirectory(prefix='tidy-apply-') as d:
            Path(d, 'fixes.yaml').write_text(replacements_yaml(chosen_of))
            p = subprocess.run(
                [tool, '--format', '--style={DisableFormat: true}', d], capture_output=True, text=True)
            if p.returncode != 0:
                raise Fatal(
                    3, f'{tool} failed: {(p.stdout + p.stderr).strip()[-600:]}')
    return applied, conflicts


def replacements_tool(clang_tidy):
    """clang-apply-replacements next to the clang-tidy in use (same release), else the one on PATH, else None."""
    sibling = os.path.join(os.path.dirname(
        os.path.realpath(clang_tidy)), 'clang-apply-replacements')
    return sibling if os.access(sibling, os.X_OK) else shutil.which('clang-apply-replacements')


# ---------------------------------------------------------------------------------------------------------------------
# NOLINT lint

NOLINT_RE = re.compile(r'\bNOLINT(NEXTLINE|BEGIN|END)?\b(\(([^)]*)\))?')


def lint_nolint_text(text, allowed):
    """[(line, kind, names, problem or None)] for each NOLINT marker of one file."""
    out = []
    for n, line in enumerate(text.splitlines(), 1):
        for m in NOLINT_RE.finditer(line):
            kind = m.group(1) or ''
            names = [x.strip()
                     for x in (m.group(3) or '').split(',') if x.strip()]
            problem = None
            if kind == '':
                problem = 'a same-line NOLINT: use `// NOLINTNEXTLINE(<check>) // <reason>` on the line before'
            elif m.group(2) is None or not names:
                problem = f'NOLINT{kind} names no check'
            elif any(c in x for x in names for c in '*?'):
                problem = f'NOLINT{kind} with a glob: name the check'
            else:
                unknown = [x for x in names if x not in allowed]
                if unknown:
                    problem = f'NOLINT{kind} names {", ".join(unknown)}: no enabled check of the config'
            if problem is None and kind != 'END':
                reason = line[m.end():].strip()
                reason = re.sub(r'\*/\s*$', '', reason).strip()
                reason = re.sub(r'^(//+|/\*|[-:;,])\s*', '', reason).strip()
                if not re.search(r'[A-Za-z]{2}', reason):
                    problem = (f'NOLINT{kind} without a reason: '
                               f'`// NOLINT{kind}(<check>) // <why this line is the exception>`')
            out.append((n, kind, names, problem))
    return out


def git_files(repo):
    p = subprocess.run(['git', '-C', repo, 'ls-files', '-co',
                       '--exclude-standard', '-z'], capture_output=True)
    if p.returncode != 0:
        return None
    return [os.path.join(repo, f) for f in p.stdout.decode(errors='replace').split('\0') if f]


def nested_checkouts(repo):
    """Real (not symlinked) checkouts below repo that are its submodules, recursively."""
    out = []
    gm = os.path.join(repo, '.gitmodules')
    if not os.path.isfile(gm):
        return out
    p = subprocess.run(['git', 'config', '-f', gm, '--get-regexp', r'^submodule\..*\.path$'],
                       capture_output=True, text=True)
    for line in p.stdout.splitlines():
        sub = os.path.join(repo, line.split(None, 1)[1])
        if os.path.islink(sub) or not os.path.exists(os.path.join(sub, '.git')):
            continue
        out.append(os.path.realpath(sub))
        out += nested_checkouts(sub)
    return out


def lint_repos(repos, owners, allowed, exclude_re, keep, tree_reals, whole_tree=False):
    """NOLINT findings (nolint-format, nolint-budget) and the file list per repo (for the STALE check).

    The budget counts the markers of one check per checkout, or over all of `repos` together with whole_tree (the
    workspace run): 15 markers of one check spread over 8 checkouts are a config question too."""
    findings, files_of = [], {}
    budget = collections.defaultdict(list)
    for repo in sorted(repos):
        files = git_files(repo)
        if files is None:
            findings.append(Finding(repo, repo, 1, 1, 'nolint-format',
                                    f'git cannot list {repo} (no checkout, or not a safe.directory): its NOLINT '
                                    f'markers are not checked', ('nolint-format',)))
            continue
        files_of[repo] = files
        for path in files:
            if os.path.splitext(path)[1] not in CXX_SUFFIXES or os.path.islink(path):
                continue
            real = os.path.realpath(path)
            if owners.owner(real) != repo:
                continue
            if is_excluded(real, exclude_re) or LIB_RE.search(real) or '/_deps/' in real:
                continue
            if any(real.startswith(t + os.sep) for t in tree_reals):
                continue
            display = owners.display(real)
            if keep_match(keep, display):
                continue
            try:
                text = Path(real).read_text(errors='replace')
            except OSError:
                continue
            if 'NOLINT' not in text:
                continue
            for line, kind, names, problem in lint_nolint_text(text, allowed):
                if problem:
                    findings.append(
                        Finding(real, real, line, 1, 'nolint-format', problem, ('nolint-format',)))
                if kind in ('NEXTLINE', 'BEGIN'):
                    for name in names:
                        budget[('' if whole_tree else repo, name)
                               ].append((repo, real, line))
    for (repo, name), sites in sorted(budget.items()):
        if len(sites) > NOLINT_BUDGET:
            _, real, line = sites[0]
            per_repo = collections.Counter(
                os.path.basename(r) for r, _, _ in sites)
            where = (', '.join(f'{r} {n}' for r, n in sorted(per_repo.items())) if whole_tree else
                     os.path.basename(repo))
            findings.append(Finding(real, real, line, 1, 'nolint-budget',
                                    f'{len(sites)} NOLINT markers name {name} in {where} (budget {NOLINT_BUDGET}'
                                    f'{" over all checkouts" if whole_tree else ""}): switch the check off or tune '
                                    f'it in the config instead', ('nolint-budget',)))
    return findings, files_of


def stale_keep(keep, files_of, owners):
    """Keep entries naming a scanned repository in which nothing matches any more."""
    names = {os.path.basename(r): r for r in files_of}
    out = []
    for e in keep:
        if e.repo is None or e.repo not in names:
            continue
        displays = (owners.display(os.path.realpath(f))
                    for f in files_of[names[e.repo]])
        if not any(e.regex.match(d) for d in displays):
            out.append(Finding(e.origin, os.path.realpath(e.origin), 1, 1, 'keep-stale',
                               f'STALE [{e.kind}] "{e.key}": nothing in {e.repo} matches any more - remove it',
                               ('keep-stale',)))
    return out


# ---------------------------------------------------------------------------------------------------------------------
# machine: RAM, jobs, the shared build lock, busy trees

def mem_available(meminfo=None):
    try:
        text = meminfo if meminfo is not None else Path(
            '/proc/meminfo').read_text()
    except OSError:
        return None
    m = re.search(r'^MemAvailable:\s+(\d+) kB', text, re.M)
    return int(m.group(1)) * 1024 if m else None


def default_jobs(meminfo=None, nproc=None, ci=None):
    nproc = nproc or os.cpu_count() or 1
    if ci if ci is not None else in_ci():
        return nproc
    avail = mem_available(meminfo)
    by_ram = MAX_JOBS if avail is None else (
        avail - MEM_RESERVE) // MEM_PER_UNIT
    return max(1, min(MAX_JOBS, nproc, by_ram))


def may_start(running, meminfo=None):
    """Admission: one process always; another only while MemAvailable >= 3 GiB."""
    if running == 0:
        return True
    avail = mem_available(meminfo)
    return avail is None or avail >= MEM_ADMIT


RELOCK_GAP_S = 2.0


class BuildLock:
    def __init__(self, enabled):
        path = Path(os.environ.get('KVASIR_BUILD_LOCK', str(
            Path.home() / '.cache' / 'kvasir_build.lock')))
        self.path = path
        self.enabled = enabled and path.exists(
        ) and not os.environ.get('KVASIR_BUILD_LOCK_HELD')
        self.fd = None
        self.since = 0.0
        self.waited = 0.0
        self.released = None

    def acquire(self):
        if not self.enabled or self.fd is not None:
            return
        # flock is not fair: re-taking the lock right after a release wins against every waiter, and a long run then
        # starves the builds queued behind it. Give them a moment to get in between batches.
        if self.released is not None:
            time.sleep(max(0.0, RELOCK_GAP_S -
                       (time.monotonic() - self.released)))
        self.fd = open(self.path, 'a')
        try:
            fcntl.flock(self.fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            eprint(f'tidy: waiting for the build lock {self.path}')
            t = time.monotonic()
            fcntl.flock(self.fd, fcntl.LOCK_EX)
            self.waited += time.monotonic() - t
        self.since = time.monotonic()

    def release(self):
        if self.fd is not None:
            fcntl.flock(self.fd, fcntl.LOCK_UN)
            self.fd.close()
            self.fd = None
            self.released = time.monotonic()

    def held_for(self):
        return time.monotonic() - self.since if self.fd is not None else 0.0


def ninjas_in(tree_real):
    """pids of ninja processes working in that tree (cwd, or -C)."""
    pids = []
    for pid in os.listdir('/proc'):
        if not pid.isdigit() or int(pid) == os.getpid():
            continue
        try:
            if Path(f'/proc/{pid}/comm').read_text().strip() != 'ninja':
                continue
            cwd = os.readlink(f'/proc/{pid}/cwd')
            argv = Path(f'/proc/{pid}/cmdline').read_bytes().split(b'\0')
        except OSError:
            continue
        d = cwd
        for i, a in enumerate(argv):
            a = a.decode(errors='replace')
            if a == '-C' and i + 1 < len(argv):
                d = os.path.join(d, argv[i + 1].decode(errors='replace'))
            elif a.startswith('-C') and len(a) > 2:
                d = os.path.join(d, a[2:])
        if os.path.realpath(d) == tree_real:
            pids.append(int(pid))
    return pids


def build_prerequisites(tree, units, wait, lock, jobs):
    if tree.generator != 'ninja':
        return
    targets = sorted(
        {u.entry.target for u in units if u.tree is tree and u.entry.target})
    if not targets:
        return
    deadline = time.monotonic() + wait
    while ninjas_in(tree.real):
        if time.monotonic() > deadline:
            raise Fatal(
                2, f'{tree.path}: a ninja is still running there after {wait} s; not building beside it')
        eprint(f'tidy: a ninja runs in {tree.path}; waiting')
        time.sleep(5)
    phonies = [f'cmake_object_order_depends_target_{t}' for t in targets]
    # the lock stays held: the first batch of units runs under the same hold
    lock.acquire()
    for i in range(0, len(phonies), 200):
        p = subprocess.run(['ninja', '-C', tree.path, f'-j{min(jobs, MAX_JOBS)}', *phonies[i:i + 200]],
                           capture_output=True, text=True)
        if p.returncode != 0:
            lock.release()
            tail = '\n'.join((p.stdout + p.stderr).splitlines()[-30:])
            raise Fatal(
                3, f'{tree.path}: building the generated-header prerequisites failed:\n{tail}')


# ---------------------------------------------------------------------------------------------------------------------
# running clang-tidy

@dataclass
class Result:
    unit: Unit
    output: str = ''
    rc: int = 0
    cpu: float = 0.0
    rss: int = 0
    cached: bool = False
    retried: bool = False
    mem_at_end: int | None = None
    fixes: str = ''


def exec_measured(cmd, cwd=None):
    """Runs cmd, stdout and stderr into one text; returns (text, exit code or -signal, CPU s, peak RSS bytes)."""
    with tempfile.TemporaryFile() as out:
        p = subprocess.Popen(
            cmd, stdout=out, stderr=subprocess.STDOUT, cwd=cwd)
        _, status, ru = os.wait4(p.pid, 0)
        p.returncode = os.waitstatus_to_exitcode(status)
        out.seek(0)
        text = out.read().decode(errors='replace')
    return text, p.returncode, ru.ru_utime + ru.ru_stime, ru.ru_maxrss * 1024


def tidy_command(ctx, unit, fixes_path=''):
    cmd = [ctx.tidy, '-p', unit.tree.db_dir,
           f'--config-file={ctx.config}', '--quiet', '--use-color=false']
    if unit.checks_arg:
        cmd.append(f'--checks={unit.checks_arg}')
    if fixes_path:
        cmd.append(f'--export-fixes={fixes_path}')
    cmd.append(unit.entry.file)
    return cmd


def run_unit(ctx, unit, fixes_path=''):
    res = Result(unit)
    cmd = tidy_command(ctx, unit, fixes_path)
    res.output, res.rc, res.cpu, res.rss = exec_measured(cmd)
    if res.rc < 0 and -res.rc in CRASH_SIGNALS and not ctx.ci:
        # a header rewritten under clang-tidy's mmap by another session: once more
        res.retried = True
        res.output, res.rc, cpu, rss = exec_measured(cmd)
        res.cpu += cpu
        res.rss = max(res.rss, rss)
    res.mem_at_end = mem_available()
    if res.rc >= 0 and ctx.use_cache:
        findings = parse_output(res.output, unit.entry.directory)
        if not compile_failed(res.output, findings):
            if unit.includes is None:
                unit.includes = depfile_deps(
                    unit.entry) or preprocess_deps(unit.entry)
            cache_store(unit, ctx.hasher, res.output)
    return res


@dataclass
class Context:
    tidy: str
    config: str
    ci: bool
    use_cache: bool
    hasher: Hasher
    jobs: int
    lock: BuildLock
    quiet: bool


def write_unit_note(fixes, unit, res):
    """--export-fixes-dir: DIR/<n>.json beside DIR/<n>.yaml, written last, so a reader takes only finished units."""
    findings = parse_output(
        res.output, unit.entry.directory) if res.rc >= 0 else []
    note = {'file': unit.entry.file, 'real': unit.real, 'tree': unit.tree.path, 'rc': res.rc,
            'compile_failed': res.rc >= 0 and compile_failed(res.output, findings),
            # clang-tidy writes no YAML for a unit without a diagnostic
            'yaml': os.path.isfile(fixes)}
    tmp = fixes[:-len('.yaml')] + '.json.tmp'
    Path(tmp).write_text(json.dumps(note))
    os.replace(tmp, fixes[:-len('.yaml')] + '.json')


def schedule(ctx, units, fix_dir='', notes=False):
    """Runs the units (cache misses) with -j processes, RAM admission and the per-batch build lock."""
    results = []
    if not units:
        return results
    pending = collections.deque(units)
    running = {}
    done = 0
    current_tree = None
    draining = False
    with cf.ThreadPoolExecutor(max_workers=ctx.jobs) as pool:
        while pending or running:
            # a batch ends at a tree boundary once the lock was held a while, and after BATCH_SECONDS in any case
            if pending and current_tree is not None and current_tree is not pending[0].tree:
                if ctx.lock.held_for() > BATCH_MIN_SECONDS:
                    draining = True
                else:
                    current_tree = pending[0].tree
            if pending and ctx.lock.held_for() > BATCH_SECONDS:
                draining = True
            if draining and not running:
                ctx.lock.release()
                draining = False
                current_tree = None
            if pending and not draining and len(running) < ctx.jobs and may_start(len(running)):
                unit = pending.popleft()
                if current_tree is None:
                    ctx.lock.acquire()
                    current_tree = unit.tree
                fixes = os.path.join(
                    fix_dir, f'{len(results) + len(running)}.yaml') if fix_dir else ''
                running[pool.submit(run_unit, ctx, unit, fixes)] = (
                    unit, fixes)
                continue
            finished, _ = cf.wait(list(running), timeout=2,
                                  return_when=cf.FIRST_COMPLETED)
            for fut in finished:
                unit, fixes = running.pop(fut)
                res = fut.result()
                res.fixes = fixes
                if notes and fixes:
                    write_unit_note(fixes, unit, res)
                results.append(res)
                done += 1
                if not ctx.quiet:
                    state = f'signal {-res.rc}' if res.rc < 0 else f'{res.cpu:6.1f} s {res.rss / GIB:4.2f} GB'
                    eprint(
                        f'[{done}/{len(units)}] {state}  {unit.display} ({unit.entry.cpu})')
    ctx.lock.release()
    return results


def live_checks(tidy, config, extra=''):
    cmd = [tidy, '--list-checks', f'--config-file={config}']
    if extra:
        cmd.append(f'--checks={extra}')
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        raise Fatal(
            2, f'clang-tidy --list-checks failed:\n{p.stdout}{p.stderr}')
    return [line.strip() for line in p.stdout.splitlines() if line.startswith('    ') and line.strip()]


def lock_diff(lock, live):
    """(new: live - lock, missing: lock - live)."""
    return sorted(set(live) - set(lock)), sorted(set(lock) - set(live))


def config_exclude(config_text):
    m = re.search(
        r"^ExcludeHeaderFilterRegex:\s*'((?:[^']|'')*)'", config_text, re.M)
    if not m:
        m = re.search(
            r'^ExcludeHeaderFilterRegex:\s*"([^"]*)"', config_text, re.M)
        return re.compile(m.group(1)) if m else None
    return re.compile(m.group(1).replace("''", "'"))


# ---------------------------------------------------------------------------------------------------------------------
# the run

def resolve_repos(paths, nested):
    out = set()
    for p in paths:
        real = os.path.realpath(p)
        if not os.path.isdir(real):
            raise Fatal(2, f'--repo {p}: no such directory')
        out.add(real)
        if nested:
            out.update(nested_checkouts(real))
    return out


def workspace_repos(root, workspace):
    if not root or not workspace or 'list' not in workspace:
        return None
    lst = Path(root) / workspace['list']
    repos = set()
    for line in lst.read_text().splitlines():
        words = line.split('#', 1)[0].split()
        if words and words[0] not in ('nolink', 'fetchlink'):
            d = os.path.realpath(os.path.join(root, words[0]))
            if os.path.exists(os.path.join(d, '.git')):
                repos.add(d)
                repos.update(nested_checkouts(d))
    return repos


def changed_files(repos, rev):
    changed = set()
    for repo in sorted(repos):
        r = rev
        if subprocess.run(['git', '-C', repo, 'rev-parse', '--verify', '--quiet', f'{rev}^{{commit}}'],
                          capture_output=True).returncode != 0:
            r = 'HEAD'
        for cmd in (['git', '-C', repo, 'diff', '--name-only', '-z', r, '--'],
                    ['git', '-C', repo, 'ls-files', '--others', '--exclude-standard', '-z']):
            p = subprocess.run(cmd, capture_output=True)
            for f in p.stdout.decode(errors='replace').split('\0'):
                if f:
                    changed.add(os.path.realpath(os.path.join(repo, f)))
    return changed


def select_changed(units, changed, hasher):
    """The units whose main file or include set (ninja deps, depfile, else the cached one) holds a changed file; a
    unit with no known include set is taken."""
    kept = []
    for u in units:
        if u.includes is None:
            cache_load(u, hasher)  # the stored include set, if any
        if u.real in changed or u.includes is None or changed.intersection(u.includes):
            kept.append(u)
    return kept


def tool_fingerprint(tidy, files):
    h = hashlib.sha256(subprocess.run(
        [tidy, '--version'], capture_output=True, text=True).stdout.encode())
    for f in files:
        try:
            h.update(Path(f).read_bytes())
        except OSError:
            h.update(b'missing')
    return h.hexdigest()


def finalize(findings, owners, trees, exclude_re, keep, repo_set, drops, foreign):
    """Drop by primary location, dedupe by real location and check, attribute; returns the counted findings."""
    tree_reals = [t.real for t in trees]
    seen, out = set(), []
    for f in findings:
        if is_excluded(f.file, exclude_re) or is_excluded(f.real, exclude_re) or LIB_RE.search(f.real) \
                or '/_deps/' in f.real:
            drops['excluded path (note leak)'] += 1
            continue
        if any(f.real.startswith(t + os.sep) for t in tree_reals) and not f.check.startswith(('nolint', 'keep')):
            drops['in a build tree'] += 1
            continue
        f.display = owners.display(f.real)
        f.owner = owners.owner(f.real)
        if keep_match(keep, f.display, ('skip',)):
            drops['keep table path'] += 1
            continue
        k = (f.real, f.line, f.col, f.check)
        if k in seen:
            drops['duplicate'] += 1
            continue
        seen.add(k)
        if repo_set is not None and f.owner not in repo_set:
            foreign[os.path.basename(f.owner) if f.owner else '(none)'] += 1
            continue
        out.append(f)
    out.sort(key=lambda f: (f.display, f.line, f.col, f.check))
    return out


def report(args, ctx, findings, results, units, drops, foreign, notices, problems, alias_errors, header):
    gh = args.format == 'github'
    by_check = collections.defaultdict(lambda: [0, set()])
    by_repo = collections.Counter()
    for f in findings:
        by_check[f.check][0] += 1
        by_check[f.check][1].add(f.real)
        by_repo[f.display.split('/', 1)[0]] += 1
    lines = [header]
    lines += [f'notice: {n}' for n in notices]
    if not args.quiet:
        for f in findings:
            lines.append(
                f'{f.display}:{f.line}:{f.col}: {f.check}: {f.message}')
            if args.notes or f.check.startswith('clang-analyzer-'):
                for path, ln, col, msg in f.notes[:12]:
                    lines.append(f'    note: {path}:{ln}:{col}: {msg}')
    if by_check:
        lines.append('checks:')
        for check, (n, files) in sorted(by_check.items(), key=lambda kv: (-kv[1][0], kv[0])):
            lines.append(f'  {n:6d} {len(files):5d} files  {check}')
        lines.append('repositories:')
        for repo, n in by_repo.most_common():
            lines.append(f'  {n:6d}  {repo}')
    dropped = ', '.join(f'{k} {v}' for k, v in sorted(drops.items()) if v)
    if dropped:
        lines.append(f'dropped: {dropped}')
    if foreign:
        lines.append('foreign (not counted): ' +
                     ', '.join(f'{n} in {r}' for r, n in foreign.most_common()))
    lines += [f'ERROR: {p}' for p in problems + alias_errors]
    print('\n'.join(lines), flush=True)
    if gh:
        ws = os.environ.get('GITHUB_WORKSPACE', '')
        for f in findings[:ANNOTATION_LIMIT]:
            path = os.path.relpath(f.real, ws) if ws and f.real.startswith(
                ws + os.sep) else f.real
            msg = f.message.replace('%', '%25').replace(
                '\r', '%0D').replace('\n', '%0A')
            print(
                f'::error file={path},line={f.line},col={f.col},title={f.check}::{msg}')
        for p in problems + alias_errors:
            print('::error title=tidy::' + p.replace('\n', '%0A')[:2000])
        if len(findings) > ANNOTATION_LIMIT:
            print(
                f'::notice title=tidy::{len(findings) - ANNOTATION_LIMIT} more findings: see the step summary')
        summary = os.environ.get('GITHUB_STEP_SUMMARY')
        if summary:
            md = ['## clang-tidy', '', header, '']
            md += [f'- notice: {n}' for n in notices]
            md += [f'- **error:** {p}' for p in problems + alias_errors]
            if by_check:
                md += ['', '| findings | files | check |', '|---:|---:|---|']
                md += [f'| {n} | {len(fs)} | `{c}` |' for c, (n, fs) in
                       sorted(by_check.items(), key=lambda kv: (-kv[1][0], kv[0]))]
                md += ['', '| findings | repository |', '|---:|---|'] + [f'| {n} | {r} |' for r, n in
                                                                         by_repo.most_common()]
                md += ['', '```']
                md += [f'{f.display}:{f.line}:{f.col}: {f.check}: {f.message}' for f in findings[:SUMMARY_LIMIT]]
                if len(findings) > SUMMARY_LIMIT:
                    md.append(f'... {len(findings) - SUMMARY_LIMIT} more')
                md.append('```')
            if dropped:
                md += ['', f'dropped: {dropped}']
            if foreign:
                md.append('foreign (not counted): ' +
                          ', '.join(f'{n} in {r}' for r, n in foreign.most_common()))
            with open(summary, 'a') as f:
                f.write('\n'.join(md) + '\n')


def checks_for(unit, args, fw_checks, new_checks):
    if args.audit:
        return f'-*,{args.audit}'
    parts = []
    if args.check:
        parts = ['-*'] + args.check.split(',')
        if unit.entry.firmware:
            return ','.join(parts)
        return ','.join(p for p in parts if p == '-*' or p not in fw_checks)
    if unit.entry.firmware:
        parts += fw_checks
    if unit.header_alone:
        parts.append('-readability-duplicate-include')
    parts += [f'-{c}' for c in new_checks]
    return ','.join(parts)


def run(args):
    config = os.path.abspath(args.config or DEFAULT_CONFIG)
    if not os.path.isfile(config):
        raise Fatal(2, f'no config at {config}')
    tidy = shutil.which(args.clang_tidy)
    if not tidy:
        raise Fatal(
            2, f'{args.clang_tidy} not found (install clang / clang-tools-extra)')
    lock_file = args.checks_lock or str(CHECKS_LOCK_FILE)
    fw_checks = read_list(FIRMWARE_CHECKS_FILE)

    if args.update_checks:
        live = live_checks(tidy, config)
        new, missing = lock_diff(read_list(lock_file), live)
        Path(lock_file).write_text('# The reviewed expansion of ../.clang-tidy (`tidy.py --update-checks`): a check a '
                                   'newer clang-tidy adds\n# under one of the globs stays off until it is listed '
                                   'here. One per line.\n' + '\n'.join(live) + '\n')

        def shown(names): return ' '.join(names) if len(
            names) <= 40 else f'({len(names)} names)'
        print(f'{lock_file}: {len(live)} checks; added {len(new)}: {shown(new) or "-"}; '
              f'removed {len(missing)}: {shown(missing) or "-"}')
        return 0
    if args.new_checks:
        new, missing = lock_diff(read_list(lock_file),
                                 live_checks(tidy, config))
        for c in new:
            print(f'new: {c}')
        for c in missing:
            print(f'gone: {c}')
        if not new and not missing:
            print(
                'the installed clang-tidy enables exactly the checks of tools/tidy_checks.txt')
        return 1 if new else 0

    p = subprocess.run([tidy, '--verify-config',
                       f'--config-file={config}'], capture_output=True, text=True)
    if p.returncode != 0 or 'error' in (p.stdout + p.stderr).lower().replace('no config errors', ''):
        raise Fatal(
            2, f'clang-tidy --verify-config: {(p.stdout + p.stderr).strip()}')
    if not args.tree:
        raise Fatal(2, 'no --tree given')
    if args.fix and args.export_fixes_dir:
        raise Fatal(
            2, '--export-fixes-dir keeps the fix-its, --fix applies them: one or the other')
    if args.fix and len(args.repo) != 1:
        raise Fatal(2, '--fix needs exactly one --repo')
    if args.fix and not replacements_tool(tidy):
        raise Fatal(
            2, 'clang-apply-replacements not found (install clang-tools-extra): --fix applies through it')

    keep_tables = [DEFAULT_KEEP] + [Path(o) for o in args.overlay]
    keep, workspace = load_keep(keep_tables)
    repo_set = resolve_repos(args.repo, args.nested) if args.repo else None
    owners = Owners(repo_set or ())
    config_text = Path(config).read_text()
    exclude_re = config_exclude(config_text)

    notices = []
    live = live_checks(tidy, config)
    lock = read_list(lock_file)
    new_checks, missing = ([], [])
    if lock:
        new_checks, missing = lock_diff(lock, live)
        if new_checks:
            notices.append(f'{len(new_checks)} check(s) of this clang-tidy are not in tools/tidy_checks.txt and stay '
                           f'off until reviewed (--update-checks): {" ".join(new_checks)}')
        if missing:
            notices.append(f'{len(missing)} check(s) of tools/tidy_checks.txt are unknown to this clang-tidy: '
                           f'{" ".join(missing)}')
    else:
        notices.append(
            'no tools/tidy_checks.txt: every check of the config runs (create it with --update-checks)')
    fw_live = set(live_checks(tidy, config, ','.join(fw_checks))
                  ) if fw_checks else set()
    unknown_fw = [c for c in fw_checks if c not in fw_live]
    if unknown_fw:
        raise Fatal(
            2, f'tools/tidy_firmware_checks.txt names checks this clang-tidy does not know: {unknown_fw}')
    enabled = (set(live) - set(new_checks)) | set(fw_checks)
    if args.check:
        bad = [c for c in args.check.split(',') if c not in enabled]
        if bad:
            raise Fatal(
                2, f'--check {",".join(bad)}: not an enabled check of the config')

    trees = [load_tree(t) for t in args.tree]
    drops = collections.Counter()
    only = [g for o in args.only for g in o.split(',') if g]
    units = select_units(trees, args.variant, keep, owners,
                         repo_set, only, exclude_re, drops)
    for u in units:
        u.checks_arg = checks_for(u, args, fw_checks, new_checks)

    ws_repos = workspace_repos(args.root, workspace)
    scope = repo_set if repo_set is not None else ws_repos if ws_repos is not None else \
        {o for o in (owners.owner(u.real) for u in units) if o}

    hasher = Hasher()
    fingerprint = tool_fingerprint(
        tidy, [config, __file__, lock_file, FIRMWARE_CHECKS_FILE])
    use_cache = not args.no_cache and not args.fix and not args.export_fixes_dir
    deps = {}
    for t in trees:
        deps.update(ninja_deps(t, units))
    for u in units:
        u.cache_id = unit_identity(fingerprint, u)
        known = deps.get(u.entry.output)
        u.includes = known if known else depfile_deps(u.entry)

    changed_mode = args.changed is not None
    if changed_mode:
        changed = changed_files(scope, args.changed or 'HEAD')
        meta = {os.path.realpath(p) for p in [
            config, __file__, lock_file, FIRMWARE_CHECKS_FILE, *keep_tables]}
        if changed & meta:
            notices.append(
                'the config, the runner or one of its tables changed: every unit is tidied')
        else:
            kept = select_changed(units, changed, hasher)
            drops['unchanged (--changed)'] += len(units) - len(kept)
            units = kept
    units = shard(units, args.shard)

    ci = in_ci()
    jobs = args.jobs or default_jobs()
    build_lock = BuildLock(not args.no_lock and not ci)
    ctx = Context(tidy, config, ci, use_cache, hasher,
                  jobs, build_lock, args.quiet)

    results, todo = [], []
    for u in units:
        out = cache_load(u, hasher) if use_cache else None
        if out is not None:
            results.append(Result(u, output=out, cached=True))
        else:
            todo.append(u)
    if todo and not args.no_build:
        for t in trees:
            build_prerequisites(t, todo, args.wait, build_lock, jobs)
    for t in trees:
        if any(u.tree is t for u in todo):
            write_db(t, todo)
    fix_dir = tempfile.mkdtemp(prefix='tidy-fixes-') if args.fix else ''
    if args.export_fixes_dir:
        ensure_dir(args.export_fixes_dir)
    t0 = time.monotonic()
    try:
        results += schedule(ctx, sorted(todo, key=lambda u: (args.tree.index(u.tree.path)
                                                             if u.tree.path in args.tree else 0, u.display)),
                            fix_dir or args.export_fixes_dir or '', notes=bool(args.export_fixes_dir))
        fixed_note = ''
        if args.fix:
            results, fixed_note = apply_fixes(
                ctx, args, results, owners, repo_set, exclude_re, keep, trees)
            if fixed_note:
                notices.append(fixed_note)
    finally:
        for t in trees:
            if t.db_dir:
                shutil.rmtree(t.db_dir, ignore_errors=True)
        if fix_dir:
            shutil.rmtree(fix_dir, ignore_errors=True)
    wall = time.monotonic() - t0

    problems, raw = [], []
    for r in results:
        u = r.unit
        if r.rc < 0:
            sig = signal.Signals(-r.rc).name if - \
                r.rc in signal.Signals._value2member_map_ else str(-r.rc)
            last = [ln for ln in r.output.splitlines(
            ) if 'While analyzing' in ln or 'Stack dump' in ln][-1:]
            problems.append(f'clang-tidy died ({sig}{", after a retry" if r.retried else ""}) on {u.display}; '
                            f'MemAvailable then {(r.mem_at_end or 0) / GIB:.1f} GiB; {" ".join(last)}\n  reproduce: '
                            f'{shlex.join(tidy_command(ctx, u))}  (database: {u.tree.path}/compile_commands.json, '
                            f'one entry per file)')
            continue
        findings = parse_output(r.output, u.entry.directory)
        if compile_failed(r.output, findings):
            first = next((f for f in findings if f.check ==
                         'clang-diagnostic-error'), None)
            where = f'{first.file}:{first.line}: {first.message}' if first else r.output.strip(
            )[-300:]
            problems.append(
                f'{u.display}: does not compile under clang-tidy ({where}) - build the tree first')
            continue
        raw += findings
    alias_errors = sorted({f'{f.file}:{f.line}: one finding under {", ".join(f.checks)}: the config enables an alias '
                           f'(switch off all but the primary)' for f in raw if len(f.checks) > 1})

    foreign = collections.Counter()
    if not (args.audit or args.check):
        lint, files_of = lint_repos(scope, owners, enabled, exclude_re, keep, [t.real for t in trees],
                                    whole_tree=repo_set is None)
        raw += lint + stale_keep(keep, files_of, owners)
    findings = finalize(raw, owners, trees, exclude_re,
                        keep, repo_set, drops, foreign)

    ran = [r for r in results if not r.cached]
    cpu = sum(r.cpu for r in ran)
    rss = max((r.rss for r in ran), default=0)
    crashed = sum(1 for r in results if r.rc < 0)
    version = subprocess.run(
        [tidy, '--version'], capture_output=True, text=True).stdout
    vm = re.search(r'version (\S+)', version)
    config_sha = hashlib.sha256(config_text.encode()).hexdigest()[:10]
    header = (f'tidy: clang-tidy {vm.group(1) if vm else "?"}, config {config_sha}, '
              f'variant {args.variant}, {len(trees)} tree(s), units {len(units)} selected / '
              f'{len(results) - len(ran)} cached / {len(ran)} run / {crashed} crashed, -j {jobs}, '
              f'CPU {cpu:.0f} s, wall {wall:.0f} s, peak RSS {rss / GIB:.2f} GB'
              + (f', waited {build_lock.waited:.0f} s for the build lock' if build_lock.waited else ''))
    report(args, ctx, findings, results, units, drops,
           foreign, notices, problems, alias_errors, header)
    if problems:
        print(f'result: could not check {len(problems)} unit(s) (exit 3)')
        return 3
    if args.audit:
        print(
            f'result: audit of {args.audit}: {len(findings)} finding(s) (exit 0)')
        return 0
    if findings or alias_errors:
        repos = len({f.display.split('/', 1)[0] for f in findings})
        print(
            f'result: {len(findings)} finding(s) in {repos} repositor{"y" if repos == 1 else "ies"} (exit 1)')
        return 1
    print('result: clean (exit 0)')
    return 0


def format_styles(files):
    """{clang-format --style value: files}: the nearest .clang-format above a file, else ~/.clang-format."""
    home = Path.home() / '.clang-format'
    groups = collections.defaultdict(list)
    for f in files:
        d, found = os.path.dirname(f), False
        while True:
            if os.path.isfile(os.path.join(d, '.clang-format')) or os.path.isfile(os.path.join(d, '_clang-format')):
                found = True
                break
            parent = os.path.dirname(d)
            if parent == d:
                break
            d = parent
        groups['file' if found or not home.is_file()
               else f'file:{home}'].append(f)
    return groups


def apply_fixes(ctx, args, results, owners, repo_set, exclude_re, keep, trees):
    """Applies the exported fix-its owned by the one --repo; returns (results with the re-tidied units, notice)."""
    repo = os.path.realpath(
        # with --nested: the named checkout only, not its submodules
        args.repo[0])
    files = git_files(repo) or []
    stamp = {}
    for f in files:
        try:
            st = os.stat(f)
            stamp[os.path.realpath(f)] = (st.st_mtime_ns, st.st_size)
        except OSError:
            pass
    per_file = collections.defaultdict(list)
    units_of_file = collections.defaultdict(set)
    tree_reals = [t.real for t in trees]
    for r in results:
        if not r.fixes or not os.path.isfile(r.fixes):
            continue
        for d in parse_fixes_yaml(Path(r.fixes).read_text(errors='replace')):
            if not d['replacements'] or not d['file']:
                continue
            reals = [os.path.realpath(
                d['file'])] + [os.path.realpath(x[0]) for x in d['replacements']]
            if any(owners.owner(x) != repo or is_excluded(x, exclude_re) or
                   keep_match(keep, owners.display(x), ('skip',)) or
                   any(x.startswith(t + os.sep) for t in tree_reals) for x in reals):
                continue
            for f, off, length, text in d['replacements']:
                per_file[os.path.realpath(f)].append(
                    (off, length, text, d['check']))
                units_of_file[os.path.realpath(f)].add(id(r.unit))
    changed_under = []
    for f in list(per_file):
        try:
            st = os.stat(f)
        except OSError:
            del per_file[f]
            continue
        if f in stamp and stamp[f] != (st.st_mtime_ns, st.st_size):
            changed_under.append(f)
            del per_file[f]
    applied, conflicts = apply_replacements(
        per_file, replacements_tool(shutil.which(args.clang_tidy)))
    touched = sorted(applied)
    if touched and shutil.which('clang-format'):
        for style, group in format_styles(touched).items():
            subprocess.run(['clang-format', '-i', f'--style={style}', *group])
    again_ids = set().union(*(units_of_file[f]
                              for f in touched)) if touched else set()
    again = [r.unit for r in results if id(r.unit) in again_ids]
    note = (f'--fix: {sum(applied.values())} replacement(s) in {len(touched)} file(s); '
            f'{len(conflicts)} overlapping fix(es) left for the next pass; '
            f'{len(again)} unit(s) tidied again')
    if changed_under:
        note += '; changed under the run, not fixed (run again): ' + ', '.join(
            owners.display(f) for f in changed_under)
    if not again:
        return results, note
    for t in trees:
        if any(u.tree is t for u in again):
            if t.db_dir:
                shutil.rmtree(t.db_dir, ignore_errors=True)
            write_db(t, again)
    second = schedule(ctx, again)
    keep_results = [r for r in results if id(r.unit) not in again_ids]
    return keep_results + second, note


def parse_args(argv):
    ap = argparse.ArgumentParser(prog='tidy.py', usage=argparse.SUPPRESS, description=__doc__, add_help=True,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--tree', action='append',
                    default=[], type=os.path.abspath)
    ap.add_argument('--repo', action='append', default=[])
    ap.add_argument('--nested', action='store_true')
    ap.add_argument('--root')
    ap.add_argument('--overlay', action='append', default=[])
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument('--changed', nargs='?', const='HEAD', default=None)
    mode.add_argument('--fix', action='store_true')
    mode.add_argument('--audit')
    mode.add_argument('--update-checks', action='store_true')
    mode.add_argument('--new-checks', action='store_true')
    mode.add_argument('--self-test', action='store_true')
    ap.add_argument('--export-fixes-dir', type=os.path.abspath)
    ap.add_argument('--check')
    ap.add_argument('--variant', default='debug')
    ap.add_argument('--only', action='append', default=[])
    ap.add_argument('--shard')
    ap.add_argument('-j', '--jobs', type=int)
    ap.add_argument('--no-build', action='store_true')
    ap.add_argument('--wait', type=int, default=600)
    ap.add_argument('--no-cache', action='store_true')
    ap.add_argument('--no-lock', action='store_true')
    ap.add_argument('--notes', action='store_true')
    ap.add_argument('--quiet', action='store_true')
    ap.add_argument('--format', choices=('text', 'github'), default='text')
    ap.add_argument('--config', help=argparse.SUPPRESS)        # fixtures
    ap.add_argument('--checks-lock', help=argparse.SUPPRESS)   # fixtures
    ap.add_argument('--clang-tidy', default=os.environ.get('KVASIR_CLANG_TIDY',
                    'clang-tidy'), help=argparse.SUPPRESS)
    return ap.parse_args(argv)


def main(argv=None):
    args = parse_args(sys.argv[1:] if argv is None else argv)
    if args.self_test:
        return self_test()
    try:
        return run(args)
    except Fatal as e:
        eprint(f'tidy: {e}')
        return e.code


# ---------------------------------------------------------------------------------------------------------------------
# self-test

def self_test():
    import contextlib
    import io
    import unittest

    def write(path, text=''):
        Path(path).parent.mkdir(parents=True, exist_ok=True)
        Path(path).write_text(text)
        return str(path)

    def git_init(d):
        subprocess.run(['git', 'init', '-q', d], check=True)

    class T(unittest.TestCase):
        def setUp(self):
            self.tmp = tempfile.mkdtemp(prefix='tidy-selftest-')

        def tearDown(self):
            shutil.rmtree(self.tmp, ignore_errors=True)

        # -- output parsing
        def test_parse_output(self):
            text = ('/w/a.hpp:3:5: warning: x [readability-braces-around-statements]\n'
                    '    3 | if (x) y;\n'
                    '/w/a.hpp:2:1: note: declared here\n'
                    '/w/b.cpp:9:1: error: m [ok] [misc-a,cppcoreguidelines-b,-warnings-as-errors]\n'
                    'rel/c.cpp:1:2: error: no member named x [clang-diagnostic-error]\n'
                    'Suppressed 3 warnings (3 in non-user code).\n')
            fs = parse_output(text, '/base')
            self.assertEqual([f.check for f in fs], ['readability-braces-around-statements', 'misc-a',
                                                     'clang-diagnostic-error'])
            self.assertEqual(
                fs[0].notes, [('/w/a.hpp', 2, 1, 'declared here')])
            self.assertEqual(fs[1].checks, ('misc-a', 'cppcoreguidelines-b'))
            self.assertEqual(fs[1].message, 'm [ok]')
            self.assertEqual(fs[2].file, '/base/rel/c.cpp')
            self.assertTrue(compile_failed(text, fs))
            self.assertFalse(compile_failed('', fs[:2]))

        # -- one command per (source, cpu), the variant's first
        def test_select_variant_and_dedupe(self):
            src = write(f'{self.tmp}/r/main.cpp')
            os.mkdir(f'{self.tmp}/r/.git')
            lib = write(f'{self.tmp}/sdk/lib/libc/src/x.cpp')
            raws = []
            for v in ('release', 'debug', 'sanitize'):
                raws.append({'directory': f'{self.tmp}/b', 'file': src, 'output': f'CMakeFiles/m_{v}.dir/main.o',
                             'command': f'clang++ -mcpu=cortex-m33 -DV={v} -c {src}'})
            raws.append({'directory': f'{self.tmp}/b', 'file': src, 'output': 'CMakeFiles/p_debug.dir/main.o',
                         'command': f'clang++ -mcpu=cortex-m0plus -c {src}'})
            raws.append({'directory': f'{self.tmp}/b', 'file': lib, 'output': 'CMakeFiles/c.dir/x.o',
                         'command': f'clang++ -mcpu=cortex-m33 -c {lib}'})
            raws.append({'directory': f'{self.tmp}/b', 'file': f'{self.tmp}/r/s.S', 'output': 'CMakeFiles/m.dir/s.o',
                         'command': 'clang -c s.S'})
            tree = Tree(f'{self.tmp}/b', f'{self.tmp}/b',
                        '', [make_entry(r) for r in raws])
            drops = collections.Counter()
            units = select_units([tree], 'debug', [],
                                 Owners(), None, [], None, drops)
            self.assertEqual(sorted((u.entry.cpu, u.entry.target) for u in units),
                             [('cortex-m0plus', 'p_debug'), ('cortex-m33', 'm_debug')])
            self.assertTrue(all(u.entry.firmware for u in units))
            self.assertEqual(drops['duplicate command'], 2)
            self.assertEqual(drops['runtime library or fetched source'], 1)
            self.assertEqual(drops['assembler source'], 1)
            # a compile-fail case (the same source, one define more) loses to the test itself, whatever its name
            cases = [make_entry({'directory': f'{self.tmp}/b', 'file': src, 'command': f'clang++ {d}-c {src}',
                                 'output': f'CMakeFiles/{n}.dir/main.o'})
                     for n, d in (('a_case_below_reach', '-DT=1 -DCASE '), ('z_main_test', '-DT=1 '))]
            picked = select_units([Tree(f'{self.tmp}/b', f'{self.tmp}/b', '', cases)], 'debug', [], Owners(), None,
                                  [], None, collections.Counter())
            self.assertEqual([u.entry.target for u in picked], ['z_main_test'])
            host = make_entry({'directory': '/b', 'file': '/x.cpp', 'command': 'clang++ -c /x.cpp',
                               'output': 'CMakeFiles/h_volatile.dir/x.o'})
            self.assertEqual((host.cpu, host.firmware, variant_rank(
                host, 'debug')), ('host', False, 0))

        # -- ownership: symlinked packages and submodule .git files
        def test_owners(self):
            a, b = f'{self.tmp}/A', f'{self.tmp}/B'
            os.makedirs(f'{a}/.git')
            write(f'{b}/.git', 'gitdir: ../A/.git/modules/B\n')
            hdr = write(f'{b}/src/x.hpp')
            os.symlink(f'{b}/src', f'{a}/linked')
            o = Owners()
            real = os.path.realpath(f'{a}/linked/x.hpp')
            self.assertEqual(o.owner(real), os.path.realpath(b))
            self.assertEqual(o.display(real), 'B/src/x.hpp')
            self.assertEqual(o.display(os.path.realpath(hdr)), 'B/src/x.hpp')
            self.assertEqual(
                Owners([f'{self.tmp}/C']).owner(f'{self.tmp}/C/d/e.hpp'), f'{self.tmp}/C')

        # -- drops by primary location, dedupe, foreign
        def test_finalize(self):
            os.makedirs(f'{self.tmp}/A/.git')
            os.makedirs(f'{self.tmp}/S/.git')
            ex = config_exclude(
                "ExcludeHeaderFilterRegex: '(svd_converter_generated|/_deps/|/lib/(libc)/)'\n")
            mk = lambda p, c='misc-x', line=1: Finding(
                p, p, line, 1, c, 'm', (c,))
            a = f'{self.tmp}/A/x.hpp'
            fs = [mk(a), mk(a), mk(a, line=2), mk(f'{self.tmp}/A/svd_converter_generated/R.hpp'),
                  mk(f'{self.tmp}/A/b/_deps/fmt/f.h'), mk(
                      f'{self.tmp}/A/build/gen.cpp'), mk(f'{self.tmp}/S/y.hpp'),
                  mk(f'{self.tmp}/A/vendor/v.hpp')]
            keep = [KeepEntry('A/vendor/**', 'skip', 'r',
                              glob_to_re('A/vendor/**'), 't')]
            trees = [Tree(f'{self.tmp}/A/build',
                          f'{self.tmp}/A/build', '', [])]
            drops, foreign = collections.Counter(), collections.Counter()
            out = finalize(fs, Owners(), trees, ex, keep, {
                           f'{self.tmp}/A'}, drops, foreign)
            self.assertEqual([(f.display, f.line)
                             for f in out], [('A/x.hpp', 1), ('A/x.hpp', 2)])
            self.assertEqual(drops['excluded path (note leak)'], 2)
            self.assertEqual(drops['in a build tree'], 1)
            self.assertEqual(drops['duplicate'], 1)
            self.assertEqual(drops['keep table path'], 1)
            self.assertEqual(foreign['S'], 1)

        # -- include sets
        def test_include_sets(self):
            deps = ('CMakeFiles/m.dir/main.o: #deps 2, deps mtime 123 (VALID)\n'
                    '    ../src/main.cpp\n    /usr/include/x.h\n\n'
                    'CMakeFiles/n.dir/n.o: deps not found\n')
            got = parse_ninja_deps(deps, '/t/b')
            self.assertEqual(got['/t/b/CMakeFiles/m.dir/main.o'], [os.path.realpath('/t/src/main.cpp'),
                                                                   os.path.realpath('/usr/include/x.h')])
            self.assertIsNone(got['/t/b/CMakeFiles/n.dir/n.o'])
            mk = 'main.o: ../src/main.cpp /a/b\\ c.hpp \\\n  /a/d.hpp\n/a/d.hpp:\n'
            self.assertEqual(parse_make_deps(mk, '/t/b'), [os.path.realpath('/t/src/main.cpp'), '/a/b c.hpp',
                                                           '/a/d.hpp'])

        def test_preprocess_deps(self):
            if not shutil.which('clang++'):
                self.skipTest('no clang++')
            h = write(f'{self.tmp}/inc/h.hpp', '#pragma once\n')
            s = write(f'{self.tmp}/s.cpp', '#include "h.hpp"\n')
            e = make_entry({'directory': self.tmp, 'file': s, 'output': f'{self.tmp}/s.o',
                            'command': f'clang++ -I{self.tmp}/inc -MD -MT s.o -MF s.o.d -o s.o -c {s}'})
            self.assertEqual(set(preprocess_deps(e)), {
                             os.path.realpath(s), os.path.realpath(h)})
            self.assertFalse(os.path.exists(f'{self.tmp}/s.o'))

        # -- cache key and content digest
        def test_cache(self):
            os.makedirs(f'{self.tmp}/b')
            h = write(f'{self.tmp}/h.hpp', 'a')
            s = write(f'{self.tmp}/s.cpp', 'b')
            e = make_entry({'directory': f'{self.tmp}/b',
                           'file': s, 'command': f'clang++ -c {s}'})
            tree = Tree(f'{self.tmp}/b', f'{self.tmp}/b', '', [e])
            u = Unit(tree, e, False, 's.cpp', includes=[os.path.realpath(h)])
            u.cache_id = unit_identity('fp1', u)
            self.assertNotEqual(u.cache_id, unit_identity('fp2', u))
            u2 = Unit(tree, e, False, 's.cpp', checks_arg='-x')
            self.assertNotEqual(u.cache_id, unit_identity('fp1', u2))
            cache_store(u, Hasher(), 'OUT')
            self.assertEqual(cache_load(u, Hasher()), 'OUT')
            write(h, 'changed')
            self.assertIsNone(cache_load(u, Hasher()))
            fresh = Unit(tree, e, False, 's.cpp', cache_id=u.cache_id)
            cache_load(fresh, Hasher())
            self.assertEqual(fresh.includes, [os.path.realpath(h)])

        # -- --changed: main file, include set, unknown include set
        def test_changed(self):
            repo = os.path.realpath(f'{self.tmp}/R')
            git_init(repo)
            for n in ('a.cpp', 'b.cpp', 'c.cpp', 'h.hpp', 'g.hpp'):
                write(f'{repo}/{n}', n)
            subprocess.run(['git', '-C', repo, 'add', '.'], check=True)
            subprocess.run(['git', '-C', repo, '-c', 'user.name=t', '-c', 'user.email=t@t', 'commit', '-qm', 'x'],
                           check=True)
            write(f'{repo}/h.hpp', 'changed')
            write(f'{repo}/new.hpp', 'untracked')
            changed = changed_files({repo}, 'HEAD')
            self.assertEqual(changed, {f'{repo}/h.hpp', f'{repo}/new.hpp'})
            os.makedirs(f'{self.tmp}/b')
            tree = Tree(f'{self.tmp}/b', f'{self.tmp}/b', '', [])

            def mk(n, inc): return Unit(tree, make_entry({'directory': f'{self.tmp}/b', 'file': f'{repo}/{n}',
                                                          'command': 'clang++ -c x'}), False, n, includes=inc,
                                        cache_id='none')
            units = [mk('a.cpp', [f'{repo}/h.hpp']),
                     mk('b.cpp', [f'{repo}/g.hpp']), mk('c.cpp', None)]
            self.assertEqual([u.display for u in select_changed(
                units, changed, Hasher())], ['a.cpp', 'c.cpp'])
            self.assertEqual(changed_files({repo}, 'no-such-rev'), changed)

        def test_checks_for(self):
            def e(fw): return Entry('/', [], '/a.cpp', '', '/a.cpp',
                                    't', 'cortex-m33' if fw else 'host', fw, {})
            a = argparse.Namespace(audit=None, check=None)
            fw = ['portability-restrict-system-includes',
                  'modernize-avoid-setjmp-longjmp']
            self.assertEqual(checks_for(Unit(None, e(True), False, ''), a, fw, ['new-x']),
                             'portability-restrict-system-includes,modernize-avoid-setjmp-longjmp,-new-x')
            self.assertEqual(checks_for(
                Unit(None, e(False), True, ''), a, fw, []), '-readability-duplicate-include')
            a.check = 'misc-a,modernize-avoid-setjmp-longjmp'
            self.assertEqual(checks_for(
                Unit(None, e(False), False, ''), a, fw, []), '-*,misc-a')
            a.audit = 'misc-no-recursion'
            self.assertEqual(checks_for(
                Unit(None, e(True), False, ''), a, fw, []), '-*,misc-no-recursion')

        # -- shards: disjoint, complete, stable
        def test_shard(self):
            def e(n): return Entry(
                '/', [], f'/s{n}.cpp', '', f'/s{n}.cpp', '', 'host', False, {})
            units = [Unit(None, e(n), False, str(n)) for n in range(10)]
            parts = [shard(units, f'{i}/3') for i in (1, 2, 3)]
            self.assertEqual(
                sorted(u.real for p in parts for u in p), sorted(u.real for u in units))
            self.assertEqual([u.real for u in shard(
                list(reversed(units)), '2/3')], [u.real for u in parts[1]])
            with self.assertRaises(Fatal):
                shard(units, '4/3')

        # -- -j and admission from /proc/meminfo
        def test_jobs(self):
            def mem(
                gib): return f'MemTotal: 1 kB\nMemAvailable: {gib * 1024 * 1024} kB\n'
            self.assertEqual(default_jobs(mem(40), 32, False), 8)
            self.assertEqual(default_jobs(mem(11), 32, False),
                             4)     # (11 - 4) / 1.75
            self.assertEqual(default_jobs(mem(2), 32, False), 1)
            self.assertEqual(default_jobs(mem(40), 4, False), 4)
            self.assertEqual(default_jobs(mem(2), 4, True), 4)
            self.assertTrue(may_start(0, mem(1)))
            self.assertFalse(may_start(1, mem(2)))
            self.assertTrue(may_start(3, mem(3)))

        # -- NOLINT forms and budget
        def test_nolint(self):
            allowed = {'misc-a', 'bugprone-b'}
            text = ('int a; // NOLINT\n'
                    '// NOLINTNEXTLINE(misc-a) // the register is written twice on purpose\n'
                    '// NOLINTNEXTLINE(misc-a)\n'
                    '// NOLINTNEXTLINE(misc-*) // glob\n'
                    '// NOLINTNEXTLINE(hicpp-x) // dead name\n'
                    '/* NOLINTNEXTLINE(bugprone-b) inside a macro on purpose */ \\\n'
                    '// NOLINTBEGIN(misc-a) // one block, one reason\n'
                    '// NOLINTEND(misc-a)\n'
                    '// NOLINTNEXTLINE\n')
            got = [(line, p is None)
                   for line, _, _, p in lint_nolint_text(text, allowed)]
            self.assertEqual(got, [(1, False), (2, True), (3, False), (4, False), (5, False), (6, True), (7, True),
                                   (8, True), (9, False)])
            repo = f'{self.tmp}/R'
            git_init(repo)
            write(f'{repo}/a.cpp', ''.join(
                f'// NOLINTNEXTLINE(misc-a) // reason {i}\nint x{i};\n' for i in range(4)))
            write(f'{repo}/b.cpp',
                  '// NOLINTNEXTLINE(bugprone-b) // reason\nint y;\n')
            fs, files_of = lint_repos(
                {os.path.realpath(repo)}, Owners(), allowed, None, [], [])
            self.assertEqual([(f.check, os.path.basename(f.file))
                             for f in fs], [('nolint-budget', 'a.cpp')])
            self.assertEqual(len(files_of[os.path.realpath(repo)]), 2)
            # the workspace run counts over every checkout: 2 + 2 markers in two checkouts are over the budget there,
            # and within it in each --repo run
            spread = []
            for n in ('S1', 'S2'):
                spread.append(os.path.realpath(f'{self.tmp}/{n}'))
                git_init(spread[-1])
                write(f'{spread[-1]}/s.cpp', ''.join(f'// NOLINTNEXTLINE(bugprone-b) // reason {i}\nint s{i};\n'
                                                     for i in range(2)))
            fs, _ = lint_repos(set(spread), Owners(), allowed,
                               None, [], [], whole_tree=True)
            self.assertEqual([f.check for f in fs], ['nolint-budget'])
            self.assertIn('S1 2, S2 2', fs[0].message)
            for r in spread:
                self.assertEqual(lint_repos(
                    {r}, Owners(), allowed, None, [], [])[0], [])

        # -- STALE keep entries
        def test_stale_keep(self):
            repo = os.path.realpath(f'{self.tmp}/R')
            git_init(repo)
            write(f'{repo}/third/x.cpp')
            keep = [KeepEntry(k, 'skip', 'r', glob_to_re(k), 'keep.toml') for k in
                    ('R/third/**', 'R/gone/**', '**/*_fail.cpp', 'Other/x/**')]
            out = stale_keep(keep, {repo: git_files(repo)}, Owners())
            self.assertEqual([f.message.split('"')[1]
                             for f in out], ['R/gone/**'])
            with self.assertRaises(Fatal):
                load_keep(
                    [write(f'{self.tmp}/k.toml', '[skip]\n"a/**" = ""\n')])
            with self.assertRaises(Fatal):
                load_keep(
                    [write(f'{self.tmp}/k.toml', '[allow]\n"a/**" = "x"\n')])
            self.assertTrue(glob_to_re('A/{x,y}/**/*.cpp').match('A/y/z.cpp'))
            self.assertFalse(glob_to_re('A/*.cpp').match('A/b/z.cpp'))

        # -- the check lock both ways
        def test_lock_diff(self):
            self.assertEqual(lock_diff(['a', 'b', 'c'], [
                             'b', 'c', 'd']), (['d'], ['a']))
            self.assertEqual(lock_diff(['a'], ['a']), ([], []))

        # -- export-fixes YAML and applying it
        def test_fixes(self):
            f = write(f'{self.tmp}/x.cpp',
                      'int f(int x) { if (x) return 1; return 0; }\n')
            y = (f"---\nMainSourceFile:  '{f}'\nDiagnostics:\n"
                 f"  - DiagnosticName:  readability-braces-around-statements\n    DiagnosticMessage:\n"
                 f"      Message:         'it''s'\n      FilePath:        '{f}'\n      FileOffset:      21\n"
                 f"      Replacements:\n        - FilePath:        '{f}'\n          Offset:          21\n"
                 f"          Length:          0\n          ReplacementText: ' {{'\n"
                 f"        - FilePath:        '{f}'\n          Offset:          31\n          Length:          0\n"
                 f"          ReplacementText: \"\\n}}\"\n    Notes:\n      - Message:         n\n"
                 f"        FilePath:        '{f}'\n        FileOffset:      0\n        Replacements:\n"
                 f"          - FilePath:        '{f}'\n            Offset:          0\n            Length:          3\n"
                 f"            ReplacementText: 'XXX'\n    Level:           Warning\n"
                 f"    BuildDirectory:  '{self.tmp}'\n"
                 f"  - DiagnosticName:  misc-const-correctness\n    DiagnosticMessage:\n      Message:         m\n"
                 f"      FilePath:        '{f}'\n      FileOffset:      0\n      Replacements:\n"
                 f"        - FilePath:        '{f}'\n          Offset:          21\n          Length:          0\n"
                 f"          ReplacementText: ' {{'\n    Level:           Warning\n...\n")
            ds = parse_fixes_yaml(y)
            self.assertEqual([d['check'] for d in ds], ['readability-braces-around-statements',
                                                        'misc-const-correctness'])
            self.assertEqual(ds[0]['replacements'], [
                             (f, 21, 0, ' {'), (f, 31, 0, '\n}')])
            per = collections.defaultdict(list)
            for d in ds:
                for rf, off, ln, tx in d['replacements']:
                    per[rf].append((off, ln, tx, d['check']))
            self.assertEqual(_yaml_scalar(
                ["a: 'one", '', "  two'"], 0, "'one")[0], 'one\ntwo')
            car = replacements_tool(shutil.which(
                'clang-tidy') or '/nonexistent/clang-tidy')
            if not car:
                if {g.strip() for g in os.environ.get('KVASIR_TEST_REQUIRE', '').split(',')} & {'clang', 'llvm', 'all'}:
                    self.fail(
                        'clang-apply-replacements missing although KVASIR_TEST_REQUIRE names clang')
                self.skipTest('no clang-apply-replacements')
            applied, conflicts = apply_replacements(per, car)
            self.assertEqual(applied, {f: 2})
            # the second diagnostic's identical insert is one replacement
            self.assertEqual(conflicts, [])
            self.assertEqual(
                Path(f).read_text(), 'int f(int x) { if (x) { return 1;\n} return 0; }\n')
            # modernize-use-ranges (23.1) deletes the second iterator and leaves the comma to the cleanup: without
            # it the result is `copy(a,, at)`, which does not compile
            g = write(f'{self.tmp}/r.cpp',
                      'void f() { std::copy(a.begin(), a.end(), at); }\n')
            applied, _ = apply_replacements({g: [(11, 9, 'std::ranges::copy', 'modernize-use-ranges'),
                                                 (21, 9, 'a', 'modernize-use-ranges'),
                                                 (31, 8, '', 'modernize-use-ranges')]}, car)
            self.assertEqual(applied, {g: 3})
            self.assertEqual(Path(g).read_text(),
                             'void f() { std::ranges::copy(a, at); }\n')
            # text that needs quoting reaches the file byte for byte
            h = write(f'{self.tmp}/q.cpp', 'int x;\n')
            apply_replacements({h: [(0, 0, '// "q" \\ \'s\'\tü\n', 'c')]}, car)
            self.assertEqual(Path(h).read_text(),
                             '// "q" \\ \'s\'\tü\nint x;\n')

        # -- the real clang-tidy on a two-file fixture
        def test_integration(self):
            tidy = shutil.which(os.environ.get(
                'KVASIR_CLANG_TIDY', 'clang-tidy'))
            clang = shutil.which('clang++')
            required = {g.strip() for g in os.environ.get(
                'KVASIR_TEST_REQUIRE', '').split(',')}
            if not tidy or not clang:
                if required & {'clang', 'llvm', 'all'}:
                    self.fail(
                        'clang-tidy or clang++ missing although KVASIR_TEST_REQUIRE names clang')
                self.skipTest('no clang-tidy')
            repo = f'{self.tmp}/repo'
            git_init(repo)
            write(f'{repo}/inc/h.hpp',
                  '#pragma once\ninline int g(int x) { if (x) return 1; return 0; }\n')
            write(f'{repo}/_deps/dep/d.hpp',
                  '#pragma once\ninline int d(int x) { if (x) return 1; return 0; }\n')
            for n in ('a', 'b'):
                write(f'{repo}/{n}.cpp', '#include "h.hpp"\n#include "d.hpp"\nint ' + n + '();\nint ' + n +
                      '() { return g(1) + d(1); }\n')
            write(
                f'{repo}/c.cpp', '// NOLINTNEXTLINE(hicpp-gone) // a dead name\nint c();\nint c() { return 1; }\n')
            tree = f'{repo}/build'
            os.makedirs(tree)
            db = [{'directory': tree, 'file': f'{repo}/{n}.cpp', 'output': f'CMakeFiles/t.dir/{n}.cpp.o',
                   'command': f'{clang} -std=c++23 -I{repo}/inc -I{repo}/_deps/dep -c {repo}/{n}.cpp '
                   f'-o CMakeFiles/t.dir/{n}.cpp.o'} for n in ('a', 'b', 'c')]
            write(f'{tree}/compile_commands.json', json.dumps(db))
            write(f'{tree}/CMakeFiles/4.0/CMakeCXXCompiler.cmake',
                  'set(CMAKE_CXX_COMPILER_ID "Clang")\n')
            cfg = write(f'{self.tmp}/cfg.yaml', "Checks: '-*,readability-braces-around-statements'\n"
                                                "HeaderFilterRegex: '.*'\nExcludeHeaderFilterRegex: '/_deps/'\n")
            lock = write(f'{self.tmp}/lock.txt',
                         'readability-braces-around-statements\n')
            out = io.StringIO()
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
                rc = main(['--tree', tree, '--repo', repo, '--config', cfg, '--checks-lock', lock, '--no-build',
                           '--no-lock', '--quiet', '-j', '2'])
            text = out.getvalue()
            self.assertEqual(rc, 1, text)
            self.assertIn('units 3 selected', text)
            self.assertIn(
                '     1     1 files  readability-braces-around-statements', text)
            self.assertIn('     1     1 files  nolint-format', text)
            self.assertIn('duplicate 1', text)
            # the cache: a second run runs nothing
            out = io.StringIO()
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
                main(['--tree', tree, '--repo', repo, '--config', cfg, '--checks-lock', lock, '--no-build',
                      '--no-lock', '--quiet'])
            self.assertIn('3 cached / 0 run', out.getvalue())
            # --export-fixes-dir: no cache, every unit's YAML kept beside its note, nothing applied
            exp = f'{self.tmp}/exported'
            out = io.StringIO()
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
                main(['--tree', tree, '--repo', repo, '--config', cfg, '--checks-lock', lock, '--no-build',
                      '--no-lock', '--quiet', '--export-fixes-dir', exp])
            self.assertIn('0 cached / 3 run', out.getvalue())
            notes = sorted(Path(exp).glob('*.json'))
            self.assertEqual(len(notes), 3)
            self.assertEqual({Path(json.loads(n.read_text())['file']).name for n in notes}, {
                             'a.cpp', 'b.cpp', 'c.cpp'})
            fixes = [d for n in notes if json.loads(n.read_text())['yaml']
                     for d in parse_fixes_yaml(n.with_suffix('.yaml').read_text())]
            self.assertEqual(
                sum(1 for d in fixes if d['file'].endswith('h.hpp')), 2)
            self.assertIn('if (x) return 1;', Path(
                f'{repo}/inc/h.hpp').read_text())
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(main(['--tree', tree, '--repo', repo, '--config', cfg, '--no-build', '--fix',
                                       '--export-fixes-dir', exp]), 2)
            # --fix: the header's fix-it applied once (two units export it), the units tidied again, nothing left
            out = io.StringIO()
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
                rc = main(['--tree', tree, '--repo', repo, '--config', cfg, '--checks-lock', lock, '--no-build',
                           '--no-lock', '--fix', '--only', '*/a.cpp,*/b.cpp'])
            self.assertIn('--fix: 2 replacement(s) in 1 file(s)',
                          out.getvalue())
            # c.cpp's dead NOLINT name is still there
            self.assertEqual(rc, 1, out.getvalue())
            self.assertRegex(
                Path(f'{repo}/inc/h.hpp').read_text(), r'if\s*\(x\)\s*\{\s*return 1;\s*\}')
            self.assertNotIn('readability-braces-around-statements',
                             out.getvalue().split('notice:')[0])
            self.assertIn('2 run', out.getvalue())
            # a gcc tree is refused
            write(f'{tree}/CMakeFiles/4.0/CMakeCXXCompiler.cmake',
                  'set(CMAKE_CXX_COMPILER_ID "GNU")\n')
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(
                    main(['--tree', tree, '--config', cfg, '--checks-lock', lock, '--no-build']), 2)

    suite = unittest.defaultTestLoader.loadTestsFromTestCase(T)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == '__main__':
    sys.exit(main())
