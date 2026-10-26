"""`profile`'s ranking: DWT_PCSR samples symbolised with inlined frames, by function, ELF symbol and source line.
Mangled names are demangled on demand, streamed: a demangled template signature can run to megabytes."""
import bisect
import collections
import json
import re
import shutil
import subprocess
import tempfile
import typing
from pathlib import Path

from cxx_names import display_name  # noqa: E402
from extract_sites import abbreviated, demangle, demanglers, find_demangler  # noqa: E402

from . import die


# Armv8-M ARM (DDI0553B.y) D1.2.68 DWT_PCSR, Armv7-M ARM (DDI0403E) C1.6.5 DEMCR / C1.8.14, Armv6-M
# (DDI0419E) the same addresses. Absent, it reads 0 (Cortex-M33 TRM r1p0 C3.2, v6-M/v7-M RAZ).
DEMCR = 0xE000EDFC       # bit 24 TRCENA
DWT_PCSR = 0xE000101C
PCSR_NONE = 0xFFFFFFFF   # halted, no sample, or Secure code without Secure debug


def short_function_names(names: set[str]) -> dict[str, str]:
    """Mangled name -> display_name() of its demangled form (profile's width, 120). A demangled template signature can
    run to megabytes (7.8 MB seen), so each is demangled once, streamed, and
    abbreviated() (remote_fmt, memoised) before it is stripped."""
    def split(n):   # a mangled name behind a prefix: __Thumbv7ABSLongThunk__ZN..., .Lswitch.table._ZN...
        m = re.search(r"(?<=[._])_Z[A-Z]", n)
        return (n[:m.start()], n[m.start():]) if m and not n.startswith("_Z") else ("", n)
    out = {n: display_name(n, 120) or "??" for n in names if not split(n)[
        1].startswith("_Z")}
    mangled = sorted({split(n)[1] for n in names} - set(out))
    memo: dict = {}
    unread = []
    if mangled:
        with tempfile.TemporaryFile("w+") as todo:
            todo.write("\n".join(mangled) + "\n")
            todo.seek(0)
            with subprocess.Popen([find_demangler()], stdin=todo, stdout=subprocess.PIPE,
                                  text=True) as demangler:
                assert demangler.stdout is not None
                for name, line in zip(mangled, demangler.stdout):
                    line = line.rstrip("\n")
                    if line.startswith("_Z"):
                        unread.append(name)
                    else:
                        out[name] = display_name(
                            abbreviated(line, memo=memo), 120)
    # what the first demangler cannot read, the other one may (remote_fmt extract_sites.py)
    for name, line in zip(unread, demangle(demanglers(find_demangler())[1:], unread)):
        out[name] = display_name(abbreviated(
            line, memo=memo) if not line.startswith("_Z") else line, 120)
    for n in names - set(out):
        prefix, rest = split(n)
        out[n] = prefix + out[rest]
    return out


def symbolize(elf: Path, addresses: typing.Iterable[int]) -> dict[int, list[dict]]:
    """Address -> inline stack (innermost first) with short function names. The output is read
    line by line and each name kept once: mangled, it is still 0.6 GB for 40 000 addresses of
    a large image."""
    tool = shutil.which("llvm-symbolizer")
    if not tool:
        die("llvm-symbolizer is needed for the inlined frames")
    frames: dict[int, list[dict]] = {}
    names: dict[str, str] = {}
    with tempfile.TemporaryFile("w+") as todo:
        todo.write("\n".join(f"0x{a:x}" for a in addresses) + "\n")
        todo.seek(0)
        with subprocess.Popen([tool, "--obj", str(elf), "--inlining", "--no-demangle",
                               "--output-style=JSON"], stdin=todo, stdout=subprocess.PIPE,
                              text=True) as symbolizer:
            assert symbolizer.stdout is not None
            for line in symbolizer.stdout:
                entry = json.loads(line)
                frames[int(entry["Address"], 16)] = [
                    {"FunctionName": names.setdefault(f["FunctionName"], f["FunctionName"]),
                     "FileName": f["FileName"], "Line": f["Line"]}
                    for f in entry.get("Symbol", [])]
    short = short_function_names(set(names))
    for stack in frames.values():
        for f in stack:
            f["FunctionName"] = short[f["FunctionName"]]
    return frames


def unique_suffixes(paths: list[str]) -> dict[str, str]:
    """Each path shortened to its fewest trailing components that no other path ends in."""
    parts = {p: Path(p).parts or ("",) for p in paths}
    out = {}
    for p, mine in parts.items():
        n = 1
        while n < len(mine) and any(q != p and parts[q][-n:] == mine[-n:] for q in parts):
            n += 1
        out[p] = "/".join(mine[-n:])
    return out


def rank_samples(counts: dict[int, int], frames: dict[int, list[dict]], top: int) -> dict[str, list[tuple[float, str]]]:
    """Samples per address + inline stacks (innermost first, llvm-symbolizer JSON) -> the top
    (percent, name) by self time, inclusive time and source line."""
    total = sum(counts.values()) or 1
    short: dict[str, str] = {}

    def strip(n):
        if n not in short:
            short[n] = display_name(n, 120)
        return short[n]

    own, inclusive, lines = collections.Counter(
    ), collections.Counter(), collections.Counter()
    for address, n in counts.items():
        stack = frames.get(address) or [
            {"FunctionName": "??", "FileName": "", "Line": 0}]
        own[strip(stack[0]["FunctionName"])] += n
        lines[(stack[0]["FileName"], stack[0]["Line"])] += n
        for name in {strip(f["FunctionName"]) for f in stack}:
            inclusive[name] += n

    def pct(c): return [(100 * n / total, k) for k, n in c.most_common(top)]
    top_lines = pct(lines)
    short = unique_suffixes([f for _, (f, _) in top_lines])
    return {"self": pct(own), "inclusive": pct(inclusive),
            "line": [(p, f"{short[f]}:{line}") for p, (f, line) in top_lines]}


def symbol_at(table: list[tuple[int, int, list[str]]], address: int) -> tuple[int, list[str]] | None:
    """(start, names) of the function whose body holds @p address, if any."""
    i = bisect.bisect_right(table, (address, float("inf"), [])) - 1
    if i >= 0 and table[i][0] <= address < table[i][0] + max(table[i][1], 2):
        return table[i][0], table[i][2]
    return None


def symbol_names(table: list[tuple[int, int, list[str]]], addresses: typing.Iterable[int]) -> dict[str, str]:
    """Mangled -> short name, for the names of every body one of @p addresses lies in only."""
    wanted = {n for a in addresses if (
        hit := symbol_at(table, a)) for n in hit[1]}
    return short_function_names(wanted) if wanted else {}


def symbol_name(names: list[str], short: dict[str, str], limit: int = 2) -> str:
    """A body's names, shortened (@p short, from symbol_names()), the first @p limit of them; a
    folded one says how many functions share it (counted by full name - shortened, `f<int>` and
    `f<bool>` are one)."""
    shown_names = sorted({short.get(n, n) for n in names}, key=len)
    shown = ", ".join(shown_names[:limit]) + \
        (", ..." if len(shown_names) > limit else "")
    return shown + (f"  [{len(set(names))} folded]" if len(set(names)) > 1 else "")


def rank_by_symbol(counts: dict[int, int], table: list[tuple[int, int, list[str]]], top: int,
                   short: dict[str, str] | None = None) -> list[tuple[float, str]]:
    """Self time by the ELF symbol the PC lies in - the linker's view, which the inline stack of
    the debug info does not give: under --icf a folded body's line table names whichever of its
    functions the linker kept, `FPBits::is_neg` for an `optional::has_value`."""
    if short is None:
        short = symbol_names(table, counts)
    total = sum(counts.values()) or 1
    own: collections.Counter = collections.Counter()
    for address, n in counts.items():
        hit = symbol_at(table, address)
        own[symbol_name(hit[1], short) if hit else "??"] += n
    return [(100 * n / total, k) for k, n in own.most_common(top)]
