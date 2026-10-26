#!/usr/bin/env python3
"""libcport's logic (tools/llvm_port/libcport is the bash driver around it): one subcommand per step, every input
an argument. WARN_FILE (the port's notice file, which bash's record() writes too) stays an environment variable.

  patch-headers PDIR OUT                 the header block of every patch under PDIR, as JSON, to OUT
  save-patches COMP STAGE COMMITTED PDIR HEADERS OWNED DIR...
                                         one patch per upstream file the submodule changed
  sync-sources CMAKE DEST [--srcdir D] [--exts .c,.S] VAR...
                                         a .cmake source list reconciled with the files on disk
  check-flags CONFIG CMAKE               libc.cmake's libc_flags against config.json
  options-md UP OURS OUT TAG             kvasir/libc-config/OPTIONS.md
  derive-config UP CFG EXCLUDE           the libc entrypoint and header lists the build uses
  owned-filter OWNED TOPDIR              rsync filter lines that keep owned files of one top-level dir out of the mirror
  owned-dead OWNED DEST                  the owned.txt lines that match no file
  patch-check PDIR                       the patches without a complete header block
"""
import argparse
import json
import os
import re
import subprocess
import sys

sys.dont_write_bytecode = True


# ---- owned.txt: one matcher for the port's filter, --save-patches and --check ------------------------------


class Owned:
    """kvasir/<component>/owned.txt: the files under lib/<component> that are Kvasir's, not upstream's - one path
    or glob per line, relative to the component, `#` comments and blank lines skipped, a trailing / = the whole
    directory. Globs mean what rsync's filter makes of them (the port's mirror and --check match through it):
    `*` and `?` stay within one path component, `**` crosses them."""

    def __init__(self, path: str | None):
        self.entries = []
        if path and os.path.exists(path):
            self.entries = [l.strip() for l in open(
                path) if l.strip() and not l.lstrip().startswith("#")]

    @staticmethod
    def _regex(glob: str) -> re.Pattern:
        out, i = "", 0
        while i < len(glob):
            if glob.startswith("**", i):
                out, i = out + ".*", i + 2
            elif glob[i] == "*":
                out, i = out + "[^/]*", i + 1
            elif glob[i] == "?":
                out, i = out + "[^/]", i + 1
            elif glob[i] == "[":
                end = glob.find("]", i + 1)
                if end < 0:
                    out, i = out + re.escape(glob[i]), i + 1
                else:
                    out, i = out + glob[i:end + 1], end + 1
            else:
                out, i = out + re.escape(glob[i]), i + 1
        return re.compile(out)

    def is_owned(self, rel: str) -> bool:
        """Whether the file `rel` (relative to the component) is owned: an entry names it or a directory above it."""
        for entry in self.entries:
            rx = self._regex(entry.rstrip("/"))
            parts = rel.split("/")
            # the file itself (not for a directory entry), or any directory above it
            if not entry.endswith("/") and rx.fullmatch(rel):
                return True
            if any(rx.fullmatch("/".join(parts[:k])) for k in range(1, len(parts))):
                return True
        return False

    def rsync_filter(self, top: str) -> list[str]:
        """The filter lines for the mirror of one top-level directory: its owned entries, anchored there."""
        return [f"- /{e[len(top) + 1:]}" for e in self.entries if e.startswith(top + "/")]

    def dead(self, dest: str) -> list[str]:
        """The entries that match nothing under `dest`."""
        import glob
        return [e for e in self.entries if not glob.glob(os.path.join(dest, e.rstrip("/")))]


# ---- patches ----------------------------------------------------------------------------------------------


def patch_header(text: str) -> str:
    """A patch's header block: everything before the first `--- a/` line (GNU patch ignores it)."""
    return "" if text.startswith("--- a/") else text.split("\n--- a/", 1)[0]


HEADER_FIELDS = ("Kvasir patch: ", "Why: ",
                 "Upstream: ", "Proof: ", "Drop when: ")


def header_problem(text: str) -> str | None:
    """None for a complete header; else what --check says after the patch's name."""
    lines = patch_header(text).split("\n")
    if not all(any(l.startswith(f) for l in lines) for f in HEADER_FIELDS):
        return ""
    if "TODO" in patch_header(text):
        return " (a TODO left in its header)"
    return None


def patch_check(pdir: str) -> None:
    """--check's NOHEADER lines: every patch says why it exists."""
    paths = sorted(os.path.join(root, f) for root, _, files in os.walk(pdir)
                   for f in files if f.endswith(".patch"))
    for path in paths:
        problem = header_problem(open(path).read())
        if problem is not None:
            print(f"   NOHEADER {os.path.relpath(path, pdir)}{problem}")


def patch_headers(pdir: str, out_file: str) -> None:
    """The header block of each patch, carried over to the patch --save-patches rewrites."""
    out = {}
    if os.path.isdir(pdir):
        for root, _, files in os.walk(pdir):
            for f in files:
                if f.endswith(".patch"):
                    head = patch_header(open(os.path.join(root, f)).read())
                    if head.strip():
                        out[os.path.relpath(os.path.join(root, f), pdir)] = head.rstrip(
                            "\n") + "\n\n"
    json.dump(out, open(out_file, "w"))


def save_patches(comp: str, stage: str, committed: str, pdir: str, headers_file: str, owned_file: str,
                 dirs: list[str]) -> None:
    headers = json.load(open(headers_file))
    is_owned = Owned(owned_file).is_owned
    written, stale = 0, []
    for d in dirs:
        for root, _, files in os.walk(os.path.join(committed, d)):
            for f in files:
                rel = os.path.relpath(os.path.join(root, f), committed)
                if is_owned(rel):
                    continue
                up = os.path.join(stage, rel)
                if not os.path.isfile(up):
                    stale.append(rel)
                    continue
                r = subprocess.run(["diff", "-u", "--label", "a/" + rel, "--label", "b/" + rel, up,
                                    os.path.join(committed, rel)], capture_output=True, text=True)
                if r.returncode == 1:
                    key = rel + ".patch"
                    out = os.path.join(pdir, key)
                    os.makedirs(os.path.dirname(out), exist_ok=True)
                    head = headers.pop(key, None)
                    if head is None:
                        head = (f"Kvasir patch: {comp} {rel}\nWhy:       TODO\nUpstream:  TODO\n"
                                f"Proof:     TODO\nDrop when: TODO\n\n")
                        print(
                            f"   NEW    {rel}  (header block has TODOs - fill it in)")
                    open(out, "w").write(head + r.stdout)
                    written += 1
                    print(f"   patch  {rel}")
    print(f"   {written} patch(es) written to {pdir}")
    for gone in sorted(headers):
        print(
            f"   GONE   {gone[:-len('.patch')]}  (no difference left: its header went with it)")
    wf = os.environ.get("WARN_FILE")
    if stale:
        for s in sorted(stale):
            print(
                f"   ?      {s}  (not upstream, not owned: stale, or add it to owned.txt)")
        open(wf, "a").write(
            f"NOTE\t{comp}: {len(stale)} committed file(s) neither upstream nor owned -- a port deletes them\n")


def sync_sources(cmake_file: str, dest_dir: str, target_vars: list[str], srcdir: str = 'src',
                 exts: list[str] = ('.cpp',)) -> None:
    """Reconcile a hand-maintained .cmake source list with the copied tree: prune entries whose file no longer
    exists, keep existing lines (active or commented) verbatim, and insert new files commented-out and tagged
    `# NEW` in alphabetical position. The first var receives new entries; all of them are pruned and count as
    listed. srcdir: the source subdir under dest (libc/libcxx: src; compiler-rt: builtins); exts: the compiled
    extensions (compiler-rt also has .S)."""

    primary = target_vars[0]
    ext_alt = '|'.join(re.escape(e.lstrip('.')) for e in exts)
    # .c must not match .cpp
    PATH_RE = re.compile(
        rf'{re.escape(srcdir)}/[A-Za-z0-9_./-]+\.(?:{ext_alt})(?![A-Za-z0-9_])')

    lines = open(cmake_file).read().split('\n')
    n = len(lines)

    # locate set(<VAR> ... ) blocks for our target vars
    var_block = {}
    i = 0
    while i < n:
        m = re.match(r'\s*set\(\s*([A-Za-z0-9_]+)', lines[i])
        if m and m.group(1) in target_vars:
            var = m.group(1)
            j = i + 1
            # end line may carry an inline "entry)"
            while j < n and ')' not in lines[j].split('#', 1)[0]:
                j += 1
            var_block[var] = (i, j)
            i = j + 1
        else:
            i += 1

    for v in target_vars:
        assert v in var_block, f"list {v} not found in {cmake_file}"

    # paths already listed across ALL target blocks
    listed = set()
    for var, (s, e) in var_block.items():
        # e+1: the ")" line may also carry the last entry
        for k in range(s + 1, e + 1):
            mm = PATH_RE.search(lines[k])
            if mm:
                listed.add(mm.group(0))

    # source files actually on disk
    disk = set()
    for root, _, files in os.walk(os.path.join(dest_dir, srcdir)):
        for f in files:
            if any(f.endswith(e) for e in exts):
                disk.add(os.path.relpath(os.path.join(
                    root, f), dest_dir).replace(os.sep, '/'))

    removed = []
    added = 0

    def merge(kept, new_lines):
        if any(p is None for p, _ in kept):      # block has non-path lines -> don't risk misordering
            return kept + new_lines
        res, ia, ib = [], 0, 0
        while ia < len(kept) and ib < len(new_lines):
            if kept[ia][0] <= new_lines[ib][0]:
                res.append(kept[ia])
                ia += 1
            else:
                res.append(new_lines[ib])
                ib += 1
        res.extend(kept[ia:])
        res.extend(new_lines[ib:])
        return res

    out = []
    i = 0
    while i < n:
        hit = next((v for v, (s, _) in var_block.items() if i == s), None)
        if hit is None:
            out.append(lines[i])
            i += 1
            continue
        s, e = var_block[hit]
        out.append(lines[s])                       # the "set(VAR" line
        close_raw = lines[e]
        cut = close_raw.split('#', 1)[0].rindex(')')
        # entry sharing the ")" line, if any
        head = close_raw[:cut]
        body = list(lines[s + 1:e])
        if head.strip():                           # inline "lastentry)" -> split paren onto its own line
            body.append(head.rstrip())
            close_line = close_raw[cut:]           # ")" (+ any trailing)
        else:
            # ")" already on its own line
            close_line = close_raw
        kept = []
        for ln in body:
            mm = PATH_RE.search(ln)
            if not mm:
                # preserve unknown line verbatim
                kept.append((None, ln))
            elif mm.group(0) in disk:
                # keep existing entry untouched
                kept.append((mm.group(0), ln))
            else:
                # drop: file gone from disk. An ACTIVE entry going away is worth a look
                # (a function moved or was renamed upstream), a commented one is not.
                removed.append((mm.group(0), not ln.lstrip().startswith('#')))
        if hit == primary:
            new_paths = sorted(disk - listed)
            added = len(new_paths)
            kept = merge(kept, [(p, f'    # {p}  # NEW') for p in new_paths])
        out.extend(ln for _, ln in kept)
        out.append(close_line)                      # the ")" line
        i = e + 1

    open(cmake_file, 'w').write('\n'.join(out))
    base = os.path.basename(cmake_file)
    active_gone = [p for p, active in removed if active]
    print(f"   {base}: +{added} new, -{len(removed)} removed ({len(active_gone)} of them were active)")
    for p in active_gone:
        print(f"     - active entry gone: {p}")
    _wf = os.environ.get('WARN_FILE')
    if _wf and (added or removed):
        bits = []
        if added:
            bits.append(
                f"{added} new source(s) added (commented '# NEW' -- review & enable)")
        if removed:
            bits.append(f"{len(removed)} stale entry(ies) pruned")
        open(_wf, 'a').write(f"NOTE\t{base}: " + "; ".join(bits) + "\n")
        if active_gone:
            open(_wf, 'a').write(
                f"WARN\t{base}: {len(active_gone)} ACTIVE source(s) gone upstream -- check what replaced them\n")


def check_flags(cfg_file: str, cmake_file: str) -> None:
    """Warn where LIBC_CONF_* values in config.json are not -D flags in libc.cmake's libc_flags: config.json drives only
    the throwaway llvm-side build, Kvasir compiles the copied sources with libc_flags (non-fatal)."""

    cmake_base = os.path.basename(cmake_file)
    _wf = os.environ.get('WARN_FILE')

    def record(level, msg):
        if _wf:
            open(_wf, 'a').write(f"{level}\t{msg}\n")
    vals = {n: o.get('value') for g in json.load(
        open(cfg_file)).values() for n, o in g.items()}

    # extract set(libc_flags ...) via quote-aware balanced-paren scan (closing ')' is at
    # end of the last line, and the math value contains its own parens inside quotes)
    text = open(cmake_file).read()
    i = text.index('(', text.index('set(libc_flags'))
    depth, in_dq, j = 0, False, i
    while j < len(text):
        c = text[j]
        if c == '"':
            in_dq = not in_dq
        elif not in_dq:
            if c == '(':
                depth += 1
            elif c == ')':
                depth -= 1
                if depth == 0:
                    break
        j += 1
    blob = '\n'.join(l.split('#', 1)[0]
                     for l in text[i + 1:j].split('\n'))  # drop comments
    toks = set(blob.replace('"', '').replace(
        "'", '').split())               # quotes removed
    toks_ns = {t.replace(' ', '') for t in toks}
    def ns(s): return str(s).replace(' ', '')

    VAL = {  # LIBC_CONF_* -> expected -D macro (value-style)
        'LIBC_CONF_ERRNO_MODE': 'LIBC_ERRNO_MODE',
        'LIBC_CONF_THREAD_MODE': 'LIBC_THREAD_MODE',
        'LIBC_CONF_QSORT_IMPL': 'LIBC_QSORT_IMPL',
        'LIBC_CONF_MATH_OPTIMIZATIONS': 'LIBC_MATH',
        'LIBC_CONF_WCTYPE_MODE': 'LIBC_CONF_WCTYPE_MODE',
        'LIBC_CONF_STRING_LENGTH_IMPL': 'LIBC_COPT_STRING_LENGTH_IMPL',
        'LIBC_CONF_FIND_FIRST_CHARACTER_IMPL': 'LIBC_COPT_FIND_FIRST_CHARACTER_IMPL',
    }
    BOOL = {  # bool -> -D present when true (absent when false)
        'LIBC_ADD_NULL_CHECKS': '-DLIBC_ADD_NULL_CHECKS',
        'LIBC_CONF_TRAP_ON_RAISE_FP_EXCEPT': '-DLIBC_TRAP_ON_RAISE_FP_EXCEPT',
        'LIBC_CONF_MATH_USE_SYSTEM_FENV': '-DLIBC_MATH_USE_SYSTEM_FENV',
    }
    warnings = 0
    for conf, dname in VAL.items():
        if conf not in vals:
            continue
        if ns(f"-D{dname}={vals[conf]}") in toks_ns:
            print(f"     OK   {dname} = {vals[conf]}")
        else:
            got = next((t for t in toks if t.startswith(
                f"-D{dname}=")), "(absent)")
            warnings += 1
            print(
                f"     WARN {dname}: config.json wants '{vals[conf]}' but libc.cmake has {got}")
    for conf, flag in BOOL.items():
        if conf not in vals:
            continue
        if bool(vals[conf]) == (flag in toks):
            print(f"     OK   {flag[2:]} {'on' if flag in toks else 'off'}")
        else:
            warnings += 1
            print(
                f"     WARN {conf}={vals[conf]} but {flag} is {'present' if flag in toks else 'absent'}")
    if 'LIBC_CONF_TIME_64BIT' in vals:  # false adds -DLIBC_TYPES_TIME_T_IS_32_BIT; true omits it
        has32 = '-DLIBC_TYPES_TIME_T_IS_32_BIT' in toks
        if bool(vals['LIBC_CONF_TIME_64BIT']) != has32:
            print(f"     OK   TIME_T {'64' if not has32 else '32'}-bit")
        else:
            warnings += 1
            print(
                f"     WARN TIME_64BIT={vals['LIBC_CONF_TIME_64BIT']} vs -DLIBC_TYPES_TIME_T_IS_32_BIT {'present' if has32 else 'absent'}")
    print("     (printf/scanf + stack-protector/frame-pointer configs are applied elsewhere, not checked)")
    print(
        f"     -> {warnings} mismatch(es); reconcile libc.cmake libc_flags" if warnings else "     -> in sync")
    if warnings:
        record(
            'WARN', f"{cmake_base}: {warnings} config/flag mismatch(es) vs config.json -- reconcile libc_flags")


def options_md(up_file: str, ours_file: str, out_file: str, tag: str) -> None:
    """kvasir/libc-config/OPTIONS.md: every option upstream's config.json knows, its default, what ours sets. Exits on an
    option ours sets that upstream no longer has."""
    up = json.load(open(up_file))
    ours = {n: o.get("value") for g in json.load(
        open(ours_file)).values() for n, o in g.items()}
    known = {n for g in up.values() for n in g}
    unknown = sorted(set(ours) - known)
    if unknown:
        sys.exit(
            f"error: config.json sets option(s) upstream {tag} no longer has: {', '.join(unknown)}")

    def fmt(v): return "`" + json.dumps(v) + "`"
    n = sum(len(g) for g in up.values())
    out = ["# LLVM libc configuration options", "",
           "Every `LIBC_CONF_*` option that can be set in `config.json` (or passed on the",
           "cmake command line as `-D<NAME>=<value>`, which overrides config.json).", "",
           f"Source: `libc/config/config.json` @ {tag}. {n} options in {len(up)} groups.",
           "Generated by libcport; **reference only - cmake does not read this file.**", "",
           "`set here` = value currently in this folder's `config.json`; blank = using upstream default.", ""]
    for group, opts in up.items():
        out += [f"## {group}", "",
                "| option | default | set here | doc |", "|---|---|---|---|"]
        for name, o in opts.items():
            here = fmt(ours[name]) if name in ours else ""
            doc = " ".join(str(o.get("doc", "")).split()).replace("|", "\\|")
            out.append(
                f"| `{name}` | {fmt(o.get('value'))} | {here.replace('|', chr(92) + '|')} | {doc} |")
        out.append("")
    open(out_file, "w").write("\n".join(out))
    print(f"   OPTIONS.md: {n} options @ {tag}")


def derive_config(up_dir: str, cfg_dir: str, exclude_file: str) -> None:
    """Upstream's baremetal/arm entrypoint and header lists minus exclude.txt (one regex a line)."""
    pats = []
    if os.path.exists(exclude_file):
        pats = [re.compile(l.strip()) for l in open(
            exclude_file) if l.strip() and not l.lstrip().startswith("#")]
    for name in ("entrypoints.txt", "headers.txt"):
        out, dropped = [], 0
        for line in open(os.path.join(up_dir, name)):
            tok = line.strip()
            if tok and any(p.fullmatch(tok) for p in pats):
                dropped += 1
                continue
            out.append(line)
        open(os.path.join(cfg_dir, name), "w").write("".join(out))
        print(f"   {name}: upstream baremetal/arm, {dropped} target(s) excluded")


def main(argv: list[str]) -> None:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name, args in (("patch-headers", ["pdir", "out"]),
                       ("save-patches", ["comp", "stage", "committed",
                        "pdir", "headers", "owned", "dirs*"]),
                       ("check-flags", ["config", "cmake"]
                        ), ("options-md", ["up", "ours", "out", "tag"]),
                       ("derive-config", ["up", "cfg", "exclude"]
                        ), ("owned-filter", ["owned", "top"]),
                       ("owned-dead", ["owned", "dest"]), ("patch-check", ["pdir"])):
        p = sub.add_parser(name)
        for a in args:
            p.add_argument(
                a.rstrip("*"), nargs="*" if a.endswith("*") else None)
    p = sub.add_parser("sync-sources")
    p.add_argument("cmake")
    p.add_argument("dest")
    p.add_argument("--srcdir", default="src")
    p.add_argument("--exts", default=".cpp")
    p.add_argument("vars", nargs="+")
    a = ap.parse_args(argv)
    if a.cmd == "patch-headers":
        patch_headers(a.pdir, a.out)
    elif a.cmd == "save-patches":
        save_patches(a.comp, a.stage, a.committed,
                     a.pdir, a.headers, a.owned, a.dirs)
    elif a.cmd == "sync-sources":
        sync_sources(a.cmake, a.dest, a.vars, a.srcdir, a.exts.split(","))
    elif a.cmd == "check-flags":
        check_flags(a.config, a.cmake)
    elif a.cmd == "options-md":
        options_md(a.up, a.ours, a.out, a.tag)
    elif a.cmd == "derive-config":
        derive_config(a.up, a.cfg, a.exclude)
    elif a.cmd == "owned-filter":
        for line in Owned(a.owned).rsync_filter(a.top):
            print(line)
    elif a.cmd == "owned-dead":
        for entry in Owned(a.owned).dead(a.dest):
            print(f"   DEAD     owned.txt: {entry}")
    elif a.cmd == "patch-check":
        patch_check(a.pdir)


if __name__ == "__main__":
    main(sys.argv[1:])
