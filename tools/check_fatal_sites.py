#!/usr/bin/env python3
"""Every fatal path calls Kvasir::Panic::beginFatal() before its log line.

A fatal site is a call of Panic::raise / raiseAt (or KVASIR_PANIC). If the code that leads to it in the same
function logs (UC_LOG_*), beginFatal() has to come before the first of those log calls: from there on a line
waits for room in the RTT ring while a host reads (uc_log LossPolicy::Counted), so the panic's own line is not
dropped by a full ring (kvasir/Util/Panic.hpp). A site whose function logs nothing needs nothing.

The scan is textual: a function is the region from the previous line that opens or closes a block in column 0
(a top-level `{`/`}` or a line ending in `{` at indentation 0) to the raise. Good enough for the SDK's style;
a false alarm is fixed by adding the call, never by weakening the check.

    check_fatal_sites.py [<dir>...]     default: the SDK's src/
Exit code: 0 every site ok, 1 a site logs before beginFatal().
"""

import re
import sys
from pathlib import Path

RAISE = re.compile(r'\bPanic::raise(At)?\s*\(')
LOG = re.compile(r'\bUC_LOG_[TDIWEC]\s*\(')
BEGIN = re.compile(r'\bbeginFatal\s*\(')
# a column-0 line that opens or closes a block
OPEN = re.compile(r'^(\S.*\{\s*$|\{|\}.*)$')


def sites(path: Path):
    lines = path.read_text(encoding='utf-8', errors='replace').splitlines()
    for i, line in enumerate(lines):
        if not RAISE.search(line) or line.lstrip().startswith('//') or 'inline void raise' in line:
            continue
        start = i
        while start > 0 and not OPEN.match(lines[start - 1]):
            start -= 1
        body = lines[start:i]
        first_log = next((k for k, l in enumerate(body) if LOG.search(
            l) and not l.lstrip().startswith('//')), None)
        if first_log is None:
            continue
        begin = next((k for k, l in enumerate(body) if BEGIN.search(
            l) and not l.lstrip().startswith('//')), None)
        if begin is None or begin > first_log:
            yield i + 1, start + first_log + 1


def main(argv):
    roots = [Path(a) for a in argv[1:]] or [
        Path(__file__).resolve().parent.parent / 'src']
    bad = 0
    for root in roots:
        for path in sorted(root.rglob('*.hpp')) if root.is_dir() else [root]:
            if path.name == 'Panic.hpp':
                continue   # defines raise/beginFatal/KVASIR_PANIC itself
            for raise_line, log_line in sites(path):
                print(
                    f'{path}:{raise_line}: a fatal site logs (line {log_line}) before Kvasir::Panic::beginFatal()')
                bad += 1
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
