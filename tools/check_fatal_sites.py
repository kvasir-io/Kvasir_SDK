#!/usr/bin/env python3
"""Every fatal path calls Kvasir::Panic::beginFatal() before its log line, and only fatal paths call it.

A new fatal site is written with KVASIR_DETAIL_FATAL_AT(cause, pc, detail, fmt, args...) or
KVASIR_DETAIL_FATAL(log, end) (kvasir/Runtime/Panic.hpp), which keep the order by construction; this scanner checks
every site, those and the hand-written ones.

The forms it knows:
- a fatal site (an end): Panic::raise / raiseAt, KVASIR_PANIC, KVASIR_DETAIL_FATAL(_AT), assert, and the helpers that
  raise for their caller - Check::Detail::failed, libstdcxxThrow, allocateOrPanic, waitPanic;
- a start: beginFatal, KVASIR_PANIC, KVASIR_DETAIL_FATAL(_AT) (a start on the macro's own line), log_assert (it
  starts, then logs);
- a log line: UC_LOG_[TDIWEC], UC_LOG_AT, and the wrappers UBSAN_REPORT, KVASIR_DETAIL_CHECK_LOG, ubsanReport,
  log_assert.
If the code that leads to a fatal site in the same function logs, a start has to come no later than the first of
those log lines (a start and a log on the same line pass: the macros, log_assert): from there on a line waits for
room in the RTT ring while a host reads (uc_log LossPolicy::Counted), so the panic's own line is not dropped by a
full ring. A site whose function logs nothing needs nothing. A `#define` body (with its continuation lines) is a
region of its own, checked by the same rule; the macro's own name in `#define NAME(` is not a use.

The reverse: a beginFatal() starts a path that ends the run - it switches the outputs off (the image's
Panic::FatalStartReaction) and leaves the log waiting for the host from then on. Its function must raise, be [[noreturn]], or say what ends the
run in a `// fatal end: <what>` comment on the line of the call or the line above it.

The third rule, the log levels of Runtime/Check.hpp's error classes: a crit line - UC_LOG_C, a UC_LOG_AT at
LogLevel::crit, KVASIR_DETAIL_CHECK_LOG(crit - comes only after a start in the same function (or
#define body), or the start's own line: crit means "this run ends". A function a fatal path alone calls (the COREFAULT
dump, the fatal UBSan report) is in ALLOW below, by file and function name, with the reason; live code that goes on is
never allowed there - it logs at error (a function lost) or warn (recovered). An ALLOW entry whose file was scanned but
has no function of that name (renamed, moved, gone) is STALE: delete or fix it.

A function is found by brace matching: comments and the contents of string and char literals are stripped first,
preprocessor lines (and their continuations) are left out, and a `{` opens a scope - not a function - after
`namespace` or `extern "C"`, or after a class head (a leading `template<...>`, attributes and access labels dropped,
it starts with struct/class/union/enum; its base list may hold anything), while every brace below it is a scope
too. A site's region is the outermost code block around it (the enclosing function, a member function inside
structs and namespaces). Known limit: a site in a lambda counts its whole enclosing function. A false alarm is fixed
in this scanner (with a fixture) or by restructuring the code, never by a beginFatal() on a path that does not end.

    check_fatal_sites.py [--report] [<dir or file>...]     default: the SDK's src/
Exit code: 0 every site ok, 1 a site logs before beginFatal(), a beginFatal() does not end its function, a crit
line is outside a fatal path, or an ALLOW entry is stale; 2 a path that does not exist. --report: print the findings,
exit 0 (a survey of a repo not yet swept).
"""

import argparse
import re
import sys
from pathlib import Path

# each form and what it is, in order: a start ('begin'), a log line ('log'), an end ('raise'); 'direct' is a
# beginFatal() call itself, the one the reverse rule asks an end of
FORMS = [
    (re.compile(r'\bbeginFatal\s*\('), ('begin', 'direct')),
    (re.compile(
        r'\bKVASIR_PANIC\s*\(|\bKVASIR_DETAIL_FATAL(_AT)?\s*\('), ('begin', 'raise')),
    (re.compile(r'\blog_assert\s*\('), ('begin', 'log')),
    (re.compile(r'\bUC_LOG_([TDIWEC]|AT)\s*\(|\bUBSAN_REPORT\s*\(|\bKVASIR_DETAIL_CHECK_LOG\s*\('
                r'|\bubsanReport\s*\('), ('log',)),
    (re.compile(r'\bPanic::raise(At)?\s*\(|(?<![\w.:])assert\s*\(|\bDetail::failed\s*\(|\blibstdcxxThrow\s*\('
                r'|\ballocateOrPanic\s*\(|\bwaitPanic\s*\('), ('raise',)),
]
# a crit line (rule 3); it is also a 'log' through the form above
CRIT = re.compile(r'\bUC_LOG_C\s*\(|\bKVASIR_DETAIL_CHECK_LOG\s*\(\s*crit\b'
                  r'|\bUC_LOG_AT\s*\(\s*(::)?(uc_log::)?LogLevel::crit\b')
FORMS.append((CRIT, ('crit',)))
# (file, function name, why): functions only a fatal path calls, which log crit for it. The file is the tail of its
# path, whole directories (matches()): the scan of the repository it lives in may start below the directory named.
ALLOW = [
    ('core/Fault.hpp', 'log', 'the COREFAULT dump: called only from Fault::HandlerFor\'s logRecord after '
                              'beginFatal()'),
    ('cortex_common/FaultLog.hpp', 'logCoreFault', 'the Armv7-M/Armv8-M COREFAULT dump: called only from a core\'s '
                                                   'core/Fault.hpp log'),
    ('Runtime/ubsan.hpp', 'ubsanFatalReport',
     'the two UBSan reports that end the run: only inside KVASIR_DETAIL_FATAL'),
]
DEFINE = re.compile(r'^\s*#\s*define\s+(\w+)')
FATAL_END = re.compile(r'//\s*fatal end:')
NAMESPACE = re.compile(r'(\bnamespace\b[^;(){}]*|\bextern\s*""\s*)$')
CLASS_HEAD = re.compile(r'^(struct|class|union|enum)\b')
LEADING = re.compile(
    r'^\s*((public|private|protected)\s*:|\[\[[^\]]*\]\]|alignas\s*\([^)]*\))\s*')


def is_scope(head):
    """A namespace, extern "C", or a class head: `template<...>`, attributes and access labels dropped, it starts
    with struct/class/union/enum - its base list may hold anything."""
    if NAMESPACE.search(head):
        return True
    h = head.strip()
    while True:
        m = LEADING.match(h)
        if m:
            h = h[m.end():]
            continue
        if h.startswith('template'):
            i = h.find('<')
            if i < 0:
                return False
            depth = 0
            for j in range(i, len(h)):
                depth += {'<': 1, '>': -1}.get(h[j], 0)
                if depth == 0:
                    h = h[j + 1:].lstrip()
                    break
            else:
                return False
            continue
        break
    return bool(CLASS_HEAD.match(h))


def strip(lines):
    """The lines with comments and literal contents removed (placeholders "" and '' kept), and a flag per line:
    True for a preprocessor line or a continuation of one."""
    out, pre = [], []
    in_block = False
    continued = False
    for raw in lines:
        s = []
        i, n = 0, len(raw)
        while i < n:
            c = raw[i]
            if in_block:
                if raw.startswith('*/', i):
                    in_block = False
                    i += 2
                else:
                    i += 1
                continue
            if raw.startswith('//', i):
                break
            if raw.startswith('/*', i):
                in_block = True
                i += 2
                continue
            if c in '"\'':
                # a raw string R"(...)" is not used in the scanned code; plain literals, escape-aware
                q = c
                i += 1
                while i < n and raw[i] != q:
                    i += 2 if raw[i] == '\\' else 1
                i += 1
                s.append(q + q)
                continue
            s.append(c)
            i += 1
        text = ''.join(s)
        is_pre = continued or text.lstrip().startswith('#')
        continued = is_pre and raw.rstrip().endswith('\\')
        out.append(text)
        pre.append(is_pre)
    return out, pre


class Function:
    def __init__(self, line, header):
        self.start = line          # line index of its opening brace
        self.header = header       # the text before the brace
        self.raises = False
        self.begins = []           # line indexes of beginFatal() calls
        self.allowed = False       # an ALLOW entry names it: its crit lines pass


def events_of(line, skip_col=None):
    """The forms on one stripped line as (column, order, kind), in source order; skip_col: a #define's own name."""
    out = []
    for rx, kinds in FORMS:
        for m in rx.finditer(line):
            if m.start() == skip_col:
                continue
            out += [(m.start(), i, k) for i, k in enumerate(kinds)]
    return sorted(out)


def scan_defines(raw, text, pre):
    """Yields ('logs', raise line, log line) for a #define body whose end comes after a log line and before a
    start. A body is the #define line and its continuations."""
    li = 0
    while li < len(text):
        m = DEFINE.match(text[li]) if pre[li] else None
        if not m:
            li += 1
            continue
        first_log = first_begin = None
        j = li
        while True:
            skip = m.start(1) if j == li else None
            for _, _, kind in events_of(text[j], skip):
                if kind == 'log' and first_log is None:
                    first_log = j
                elif kind == 'begin' and first_begin is None:
                    first_begin = j
                elif kind == 'crit' and first_begin is None:
                    yield 'crit', j + 1, 0
                elif kind == 'raise' and first_log is not None and (first_begin is None or first_begin > first_log):
                    yield 'logs', j + 1, first_log + 1
            if not raw[j].rstrip().endswith('\\') or j + 1 >= len(text):
                break
            j += 1
        j += 1
        li = j


def matches(file, path: Path, rel: str) -> bool:
    """An ALLOW file is this one: the path ends with it, or it ends with the path below the scan root (rel: a
    repository scanned from its own root names core_cortex_common's FaultLog.hpp without its directory)."""
    p = '/' + path.as_posix()
    return p.endswith('/' + file) or ('/' + file).endswith('/' + rel)


def entries_of(path: Path, rel: str, fn):
    """The ALLOW entries naming this function: its header names it (`Name(`) and the file matches."""
    return [e for e in ALLOW if matches(e[0], path, rel) and re.search(r'\b' + e[1] + r'\s*\(', fn.header)]


def scan(path: Path, found=None, rel=None):
    """Yields (kind, line, other) per finding: ('logs', raise line, log line) and ('begin', beginFatal line, 0); the
    ALLOW entries whose function is in this file go into `found`. rel: the path below the scan root (default: the
    name)."""
    found = set() if found is None else found
    rel = path.name if rel is None else rel
    raw = path.read_text(encoding='utf-8', errors='replace').splitlines()
    text, pre = strip(raw)
    yield from scan_defines(raw, text, pre)
    stack = []                 # 'scope' or a Function
    header = []                # the text since the last ; { }
    # (line) of the current function's first log line
    first_log = None
    first_begin = None
    for li, line in enumerate(text):
        if pre[li]:
            continue
        events = [(col, kind) for col, _, kind in events_of(line)]
        ev = 0
        for col, c in enumerate(line):
            while ev < len(events) and events[ev][0] == col:
                kind = events[ev][1]
                ev += 1
                fn = next((f for f in stack if isinstance(f, Function)), None)
                if fn is None:
                    continue
                if kind == 'log':
                    if first_log is None:
                        first_log = li
                elif kind == 'direct':
                    fn.begins.append(li)
                elif kind == 'begin':
                    if first_begin is None:
                        first_begin = li
                elif kind == 'crit':
                    if first_begin is None and not fn.allowed:
                        yield 'crit', li + 1, 0
                else:
                    fn.raises = True
                    if first_log is not None and (first_begin is None or first_begin > first_log):
                        yield 'logs', li + 1, first_log + 1
            if c == '{':
                head = ''.join(header).strip()
                if all(f == 'scope' for f in stack) and is_scope(head):
                    stack.append('scope')
                elif all(f == 'scope' for f in stack):
                    fn = Function(li, head)
                    entries = entries_of(path, rel, fn)
                    fn.allowed = bool(entries)
                    found.update(entries)
                    stack.append(fn)
                    first_log = None
                    first_begin = None
                else:
                    stack.append('code')
                header = []
            elif c == '}':
                top = stack.pop() if stack else None
                if isinstance(top, Function):
                    yield from finish(top, raw)
                header = []
            elif c == ';':
                header = []
            else:
                header.append(c)
        header.append(' ')
    for f in stack:
        if isinstance(f, Function):
            yield from finish(f, raw)


def finish(fn, raw):
    if fn.raises or 'noreturn' in fn.header:
        return
    for b in fn.begins:
        marked = FATAL_END.search(raw[b]) or (
            b > 0 and FATAL_END.search(raw[b - 1]))
        if not marked:
            yield 'begin', b + 1, 0


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--report', action='store_true',
                    help='print the findings, exit 0')
    ap.add_argument('paths', nargs='*', type=Path,
                    metavar='<dir or file>', help="default: the SDK's src/")
    args = ap.parse_args(argv[1:])
    roots = args.paths or [Path(__file__).resolve().parent.parent / 'src']
    for root in roots:
        if not root.exists():
            print(f'check_fatal_sites: {root} does not exist', file=sys.stderr)
            return 2
    bad = 0
    found = set()
    scanned = []
    for root in roots:
        for path in sorted(root.rglob('*.hpp')) if root.is_dir() else [root]:
            if path.name == 'Panic.hpp':
                continue   # defines raise/beginFatal/KVASIR_PANIC itself
            rel = path.relative_to(root).as_posix(
            ) if root.is_dir() else path.name
            scanned.append((path, rel))
            for kind, line, other in scan(path, found, rel):
                if kind == 'logs':
                    print(
                        f'{path}:{line}: a fatal site logs (line {other}) before Kvasir::Panic::beginFatal()')
                elif kind == 'crit':
                    print(f'{path}:{line}: crit log outside a fatal path (no Kvasir::Panic::beginFatal() before it in '
                          f'this function; error = a function lost, warn = recovered: Runtime/Check.hpp)')
                else:
                    print(f'{path}:{line}: Kvasir::Panic::beginFatal() in a function that neither raises nor is '
                          f'[[noreturn]] (say what ends the run: // fatal end: <what>)')
                bad += 1
    for entry in ALLOW:
        file, name, _ = entry
        if entry not in found and any(matches(file, path, rel) for path, rel in scanned):
            print(f'{Path(__file__).name}: ALLOW entry ({file}, {name}) is stale: the scanned {file} has no function '
                  f'{name}()')
            bad += 1
    return 1 if bad and not args.report else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
