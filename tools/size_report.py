#!/usr/bin/env python3
"""size_report: where the bytes of Kvasir firmware images go, and what a change did to them.

Reads ELF files that are already built (llvm-readelf, llvm-objdump); builds nothing, touches no
tree.

  size_report.py snapshot -o before.json  fw_a/build_pico            fw_b/build
  size_report.py snapshot -o after.json   pico=<scratch>/fw_a_pico  fw_b=<scratch>/fw_b
  size_report.py report   before.json                     # one markdown row per image
  size_report.py diff     before.json after.json          # per image, totals, symbols that moved
  size_report.py symbols  before.json 'fw_b/build/release' --top 40

A TREE is a build directory (searched for ELFs) or one ELF. An image's key is `<label>/<path of
the ELF in the tree, without .elf>`; the label is `name=` in front of the path, or the path's last
two components - give both sides of an A/B the same labels. `--variants` picks the images by the
end of their name (default `release,release_log`; a plain `release.elf` counts).

Per image a snapshot holds
  sections   every allocated section; `flash` = the PROGBITS ones, `ram` = .data .bss .noInit*
             (the stack takes the rest of RAM by design and is left out)
  buckets    symbol sizes by origin (libc, compiler-rt, libc++, logging, startup, usb, kvasir, app,
             outlined, thunk), code and objects apart; aliases at one address count once.
             Inlined code counts toward its caller. `unnamed` = flash bytes no sized symbol
             covers: merged strings, padding, and assembly without .size (the RP2040's
             divider.S, 1152 bytes)
  calls      call sites (bl/b to the symbol) of the helpers worth watching: atomic shims,
             unaligned read/write, division, memcpy/memset/memmove, outlined functions; and how
             many functions carry a stack canary
  symbols    name -> bytes, for `diff` and `symbols` (megabytes for a big tree; --no-symbols
             leaves them out)
"""
import argparse
import concurrent.futures
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

SKIP_DIRS = ("CMakeFiles", "_deps", "host_build", "__pycache__")
RAM_SECTIONS = re.compile(r"^\.(data|bss|noInit\w*)$")

# first match wins; matched against the name without template arguments and parameters
BUCKETS = (
    ("outlined", re.compile(r"^OUTLINED_FUNCTION_")),
    ("thunk", re.compile(r"^__Thumb\w*Thunk|^__\w+_veneer$")),
    ("libc", re.compile(r"^__llvm_libc::|^(mem(cpy|set|move|cmp|chr|rchr)|bcmp|bzero|str\w+|"
                        r"__aeabi_mem\w+|malloc|free|calloc|realloc|qsort|abs|labs)$")),
    ("compiler-rt", re.compile(
        r"^__(aeabi_|wrap___aeabi_|real___aeabi_|u?div|u?mod|mul[sdtx]|add[sdt]|sub[sdt]|neg|"
        r"clz|ctz|ffs|fix|float|cmp|eq[sd]|ne[sd]|lt[sd]|le[sd]|gt[sd]|ge[sd]|unord|extend|"
        r"trunc|ashl|ashr|lshr|popcount|bswap|parity|compiler_rt)|^(div|divmod)_[su]\d+[su]\d+")),
    ("libc++", re.compile(r"^std::|^operator (new|delete)")),
    ("logging", re.compile(r"^(uc_log|remote_fmt|rtt|sc)::")),
    ("startup", re.compile(r"^Kvasir::(Startup|StartUp|Panic|Fault|Boot|Trace|StackProtector|"
                           r"Nvic::DefaultIsrs)\b|^(ResetISR|abort|exit|_Exit|atexit|"
                           r"__stack_chk_\w+|__aeabi_unwind_cpp_pr\d)$")),
    ("usb", re.compile(r"^Kvasir::USB\b|\bUSB::")),
    ("kvasir", re.compile(r"^Kvasir::")),
)
BUCKET_NAMES = tuple(b for b, _ in BUCKETS) + ("app",)

# call-site groups: name -> regex on the (mangled) branch target
CALLS = (
    ("atomic", re.compile(r"^__(atomic|sync)_\w+$")),
    ("unaligned", re.compile(r"^__aeabi_u(read|write)[48]$")),
    # the RP2040's hardware-divider entry points are listed under their divider.S names
    ("div", re.compile(r"^(__wrap_)?__aeabi_u?l?i?div(mod)?$|^__u?div(mod)?[sd]i[34]$|"
                       r"^div(mod)?_[su](32|64)[su](32|64)$")),
    ("memcpy", re.compile(r"^(memcpy|__aeabi_memcpy[48]?)$")),
    ("memset", re.compile(r"^(memset|__aeabi_mem(set|clr)[48]?)$")),
    ("memmove", re.compile(r"^(memmove|__aeabi_memmove[48]?)$")),
    ("outlined", re.compile(r"^OUTLINED_FUNCTION_\d+$")),
)


def tool(name: str) -> str:
    for cand in (f"llvm-{name}", f"arm-none-eabi-{name}", name):
        if shutil.which(cand):
            return cand
    sys.exit(f"size_report: no llvm-{name} / arm-none-eabi-{name} in PATH")


def run(*cmd: str) -> str:
    return subprocess.run(cmd, check=True, capture_output=True, text=True,
                          errors="replace").stdout


# spellings with <, > or ( that are part of a name, not template arguments or parameters
KEPT_TOKENS = (("(anonymous namespace)", "\x01"), ("operator()", "operator\x02"),
               ("operator<=>", "operator\x03"), ("operator<<", "operator\x04"),
               ("operator>>", "operator\x05"), ("operator->", "operator\x06"),
               ("operator<", "operator\x07"), ("operator>", "operator\x08"))


def plain_name(name: str) -> str:
    """`void a::B<c<int>, 3>::f<x>(d<e>, int) const` -> `a::B::f`: no template arguments, no
    parameters, no return type. `(anonymous namespace)`, `operator()` and lambdas stay."""
    for keep, token in KEPT_TOKENS:
        name = name.replace(keep, token)
    out, angle = [], 0
    for c in name:
        if c == "<":
            angle += 1
        elif c == ">":
            angle = max(0, angle - 1)
        elif angle == 0:
            out.append(c)
    name = "".join(out)
    # parameters: cut at the first '(' that is not a lambda's `{lambda(...)#1}`
    depth_brace = 0
    for i, c in enumerate(name):
        if c == "{":
            depth_brace += 1
        elif c == "}":
            depth_brace -= 1
        elif c == "(" and depth_brace == 0:
            name = name[:i]
            break
    name = name.strip()
    if " " in name and not name.startswith("operator"):
        # a return type in front (`void f`), but not `operator new` / `unsigned int` casts
        head, _, tail = name.rpartition(" ")
        if not head.endswith("operator"):
            name = tail
    for keep, token in KEPT_TOKENS:
        name = name.replace(token, keep)
    return name


def bucket_of(name: str) -> str:
    plain = plain_name(name)
    for bucket, rx in BUCKETS:
        if rx.search(plain):
            return bucket
    return "app"


SECTION_RX = re.compile(
    r"^\s*\[\s*(\d+)\]\s+(\S+)\s+(\S+)\s+([0-9a-f]+)\s+[0-9a-f]+\s+([0-9a-f]+)\s+\S+\s+"
    r"([A-Za-z]*)\s*\d+\s+\d+\s+\d+\s*$")
SYMBOL_RX = re.compile(
    r"^\s*\d+:\s+([0-9a-f]+)\s+(0x[0-9a-f]+|\d+)\s+(\w+)\s+\w+\s+\w+\s+(\S+)\s?(.*)$")


def parse_sections(text: str) -> dict[int, dict]:
    """`llvm-readelf -S -W` -> index -> {name, type, addr, size, flags}, allocated ones only."""
    out = {}
    for line in text.splitlines():
        m = SECTION_RX.match(line)
        if m and "A" in m.group(6):
            out[int(m.group(1))] = {"name": m.group(2), "type": m.group(3),
                                    "addr": int(m.group(4), 16), "size": int(m.group(5), 16),
                                    "flags": m.group(6)}
    return out


def parse_symbols(text: str) -> list[tuple[int, int, str, int, str]]:
    """`llvm-readelf -s -W -C` -> (address, size, type, section index, name) of sized FUNC/OBJECT
    symbols; the Thumb bit is taken off a function's address."""
    out = []
    for line in text.splitlines():
        m = SYMBOL_RX.match(line)
        if not m or m.group(3) not in ("FUNC", "OBJECT") or not m.group(4).isdigit():
            continue
        size = int(m.group(2), 0)
        if size == 0:
            continue
        addr = int(m.group(1), 16)
        if m.group(3) == "FUNC":
            addr &= ~1
        out.append((addr, size, m.group(3), int(
            m.group(4)), m.group(5).strip()))
    return out


LABEL_RX = re.compile(r"^[0-9a-f]+ <(.+)>:$")
BRANCH_RX = re.compile(
    r"^\s*[0-9a-f]+:\s+(?:bl|blx|b|b\.w)\s+0x[0-9a-f]+\s+<([^>+]+)>")


def parse_calls(text: str) -> dict:
    """`llvm-objdump -d --no-show-raw-insn` -> call sites per CALLS group, per atomic/unaligned
    helper, and `canary_functions` = functions that call __stack_chk_fail."""
    counts: dict[str, int] = {name: 0 for name, _ in CALLS}
    detail: dict[str, int] = {}
    canary, current = set(), ""
    for line in text.splitlines():
        if (m := LABEL_RX.match(line)):
            current = m.group(1)
            continue
        if not (m := BRANCH_RX.match(line)):
            continue
        target = m.group(1)
        if target == current:
            continue   # a loop's branch to its own function label
        if target == "__stack_chk_fail":
            canary.add(current)
            continue
        for name, rx in CALLS:
            if rx.match(target):
                counts[name] += 1
                if name in ("atomic", "unaligned", "div"):
                    detail[target] = detail.get(target, 0) + 1
                break
    counts["canary_functions"] = len(canary)
    return {**counts, "detail": dict(sorted(detail.items()))}


def analyse(sections: dict[int, dict], symbols: list, calls: dict) -> dict:
    flash = sum(s["size"]
                for s in sections.values() if s["type"] == "PROGBITS")
    ram = sum(s["size"]
              for s in sections.values() if RAM_SECTIONS.match(s["name"]))
    buckets = {b: {"code": 0, "data": 0} for b in BUCKET_NAMES}
    by_name: dict[str, int] = {}
    ram_objects: dict[str, int] = {}
    covered: dict[int, list[tuple[int, int]]] = {}
    seen = set()
    for addr, size, kind, index, name in symbols:
        section = sections.get(index)
        if section is None or (addr, size) in seen:
            continue   # not loaded, or an alias of a symbol already counted
        seen.add((addr, size))
        by_name[name] = by_name.get(name, 0) + size
        if section["type"] == "PROGBITS":
            buckets[bucket_of(name)]["code" if kind ==
                                     "FUNC" else "data"] += size
            covered.setdefault(index, []).append((addr, addr + size))
        if kind == "OBJECT" and RAM_SECTIONS.match(section["name"]):
            ram_objects[f'{section["name"]} {name}'] = size
    named = 0
    for ranges in covered.values():   # union, so overlapping symbols count once
        end = -1
        for lo, hi in sorted(ranges):
            if hi > end:
                named += hi - max(lo, end)
                end = hi
    top_ram = dict(sorted(ram_objects.items(), key=lambda kv: -kv[1])[:15])
    return {
        "flash": flash, "ram": ram,
        "sections": {s["name"]: s["size"] for s in sections.values()},
        "buckets": buckets, "unnamed": flash - named,
        "calls": calls, "ram_objects": top_ram, "symbols": by_name,
    }


def analyse_elf(elf: Path) -> dict:
    sections = parse_sections(run(tool("readelf"), "-S", "-W", str(elf)))
    symbols = parse_symbols(run(tool("readelf"), "-s", "-W", "-C", str(elf)))
    calls = parse_calls(
        run(tool("objdump"), "-d", "--no-show-raw-insn", str(elf)))
    return analyse(sections, symbols, calls)


def find_images(spec: str, variants: tuple[str, ...]) -> dict[str, Path]:
    label, sep, path = spec.partition("=")
    if not sep:
        label, path = "", spec
    root = Path(path).resolve()
    if not root.exists():
        sys.exit(f"size_report: {path} does not exist")
    if not label:
        label = "/".join(root.parts[-2:]
                         ) if root.is_dir() else root.parent.name
    if root.is_file():
        return {f"{label}/{root.stem}": root}
    wanted = re.compile(r"(^|_)(" + "|".join(map(re.escape, variants)) + r")$")
    found = {}
    for elf in sorted(root.rglob("*.elf")):
        rel = elf.relative_to(root)
        if any(part in SKIP_DIRS or part.endswith("_host_build") for part in rel.parts[:-1]):
            continue
        if elf.stem.endswith("_flash") or not wanted.search(elf.stem):
            continue
        found[f"{label}/{rel.with_suffix('')}"] = elf
    if not found:
        sys.exit(f"size_report: no {'/'.join(variants)} ELF in {root}")
    return found


def cmd_snapshot(args: argparse.Namespace) -> None:
    images: dict[str, Path] = {}
    for spec in args.trees:
        for key, elf in find_images(spec, tuple(args.variants.split(","))).items():
            if key in images:
                sys.exit(
                    f"size_report: two images are called {key} - give the trees labels")
            images[key] = elf
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        results = dict(zip(images, pool.map(analyse_elf, images.values())))
    if args.no_symbols:
        for image in results.values():
            image["symbols"] = {}
    Path(args.output).write_text(json.dumps(
        {"images": results}, indent=1, sort_keys=True))
    print(f"{len(results)} images -> {args.output}")


def load(path: str) -> dict[str, dict]:
    return json.loads(Path(path).read_text())["images"]


def table(header: list[str], rows: list[list]) -> str:
    def cell(v) -> str:
        return f"{v:,}" if isinstance(v, int) else str(v)
    lines = ["| " + " | ".join(header) + " |",
             "|" + "|".join("---" if i == 0 else "---:" for i in range(len(header))) + "|"]
    lines += ["| " + " | ".join(cell(v) for v in row) + " |" for row in rows]
    return "\n".join(lines)


def bucket_total(image: dict, bucket: str) -> int:
    return image["buckets"][bucket]["code"] + image["buckets"][bucket]["data"]


def cmd_report(args: argparse.Namespace) -> None:
    images = load(args.snapshot)
    rx = re.compile(args.filter) if args.filter else None
    rows = []
    for key, im in images.items():
        if rx and not rx.search(key):
            continue
        c = im["calls"]
        rows.append([key, im["flash"], im["sections"].get(".text", 0), im["ram"]]
                    + [bucket_total(im, b) for b in BUCKET_NAMES]
                    + [im["unnamed"], c["atomic"], c["unaligned"], c["div"],
                       c["memcpy"] + c["memset"] + c["memmove"], c["outlined"],
                       c["canary_functions"]])
    print(table(["image", "flash", ".text", "ram"] + list(BUCKET_NAMES)
                + ["unnamed", "atomic calls", "unaligned calls", "div calls", "mem* calls",
                   "outlined calls", "canary fns"], rows))


def signed(v: int) -> str:
    return f"{v:+,}" if v else "0"


def cmd_diff(args: argparse.Namespace) -> None:
    a, b = load(args.before), load(args.after)
    rx = re.compile(args.filter) if args.filter else None
    keys = [k for k in a if k in b and (not rx or rx.search(k))]
    rows, total_a, total_b = [], 0, 0
    moved: dict[str, int] = {}
    for key in keys:
        ia, ib = a[key], b[key]
        total_a += ia["flash"]
        total_b += ib["flash"]
        delta = ib["flash"] - ia["flash"]
        calls = ", ".join(f"{n} {signed(ib['calls'][n] - ia['calls'][n])}"
                          for n in ("atomic", "unaligned", "div", "memcpy", "memset",
                                    "outlined", "canary_functions")
                          if ib["calls"][n] != ia["calls"][n])
        buckets = ", ".join(f"{n} {signed(bucket_total(ib, n) - bucket_total(ia, n))}"
                            for n in BUCKET_NAMES if bucket_total(ib, n) != bucket_total(ia, n))
        if delta or calls or buckets or ib["ram"] != ia["ram"] or args.all:
            rows.append([key, ia["flash"], ib["flash"], signed(delta),
                         f"{100 * delta / ia['flash']:+.1f} %" if ia["flash"] else "",
                         signed(ib["ram"] - ia["ram"]), buckets, calls])
        for name in ia["symbols"].keys() | ib["symbols"].keys():
            d = ib["symbols"].get(name, 0) - ia["symbols"].get(name, 0)
            if d:
                moved[name] = moved.get(name, 0) + d
    print(f"{len(keys)} images in both, {len(rows)} differ"
          + (f"; only in before: {len(a.keys() - b.keys())}, only in after: "
             f"{len(b.keys() - a.keys())}" if a.keys() != b.keys() else ""))
    print(f"flash total {total_a:,} -> {total_b:,} ({signed(total_b - total_a)}"
          + (f", {100 * (total_b - total_a) / total_a:+.2f} %" if total_a else "") + ")\n")
    if rows:
        print(table(["image", "before", "after", "flash", "%", "ram", "buckets", "call sites"],
                    rows))
    if moved and args.top:
        ranked = sorted(moved.items(), key=lambda kv: -abs(kv[1]))[:args.top]
        print("\nsymbols that moved most, summed over the images:\n")
        print(table(["symbol", "bytes"], [[n[:140], signed(d)]
              for n, d in ranked]))


def cmd_symbols(args: argparse.Namespace) -> None:
    images = load(args.snapshot)
    if args.image not in images:
        near = [k for k in images if args.image in k]
        sys.exit(f"size_report: no image {args.image}" + (f"; did you mean {near[:8]}" if near
                                                          else ""))
    symbols = images[args.image]["symbols"]
    rx = re.compile(args.grep) if args.grep else None
    ranked = sorted(((n, s) for n, s in symbols.items() if not rx or rx.search(n)),
                    key=lambda kv: -kv[1])[:args.top]
    print(table(["symbol", "bytes", "bucket"], [
          [n[:140], s, bucket_of(n)] for n, s in ranked]))


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("snapshot")
    p.add_argument("-o", "--output", required=True)
    p.add_argument("--variants", default="release,release_log")
    p.add_argument("-j", "--jobs", type=int, default=8)
    p.add_argument("--no-symbols", action="store_true",
                   help="leave the per-symbol sizes out: a snapshot small enough to keep (diff "
                        "then lists no symbols)")
    p.add_argument("trees", nargs="+", metavar="[LABEL=]TREE")
    p.set_defaults(func=cmd_snapshot)
    p = sub.add_parser("report")
    p.add_argument("snapshot")
    p.add_argument("--filter", help="regex on the image key")
    p.set_defaults(func=cmd_report)
    p = sub.add_parser("diff")
    p.add_argument("before")
    p.add_argument("after")
    p.add_argument("--filter", help="regex on the image key")
    p.add_argument("--all", action="store_true",
                   help="also list the images that did not change")
    p.add_argument("--top", type=int, default=25,
                   help="symbols to list (0: none)")
    p.set_defaults(func=cmd_diff)
    p = sub.add_parser("symbols")
    p.add_argument("snapshot")
    p.add_argument("image")
    p.add_argument("--top", type=int, default=40)
    p.add_argument("--grep", help="regex on the symbol name")
    p.set_defaults(func=cmd_symbols)
    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
