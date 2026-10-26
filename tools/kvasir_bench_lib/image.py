"""The tree's ELF: one `Image` per ELF, and each external tool run at most once per kind for it."""
import functools
import re
import shutil
import subprocess
import typing
from pathlib import Path

import kvasir_elf  # noqa: E402  cmake/tools: the post-link tools' ELF reader (read from, never extended here)
from cxx_names import display_name  # noqa: E402  tools/: the one C++-name shortener

from . import die

SHF_EXECINSTR = 0x4


def tool(name: str) -> str:
    """The binutils-like tool `name` (nm, addr2line, ...): llvm-<name>, arm-none-eabi-<name> or <name>, the first
    in PATH (size_report's order); none at all is said as such, not as a printer error."""
    for candidate in (f"llvm-{name}", f"arm-none-eabi-{name}", name):
        found = shutil.which(candidate)
        if found:
            return found
    die(f"no llvm-{name} / arm-none-eabi-{name} / {name} in PATH")


def nm_text(elf: Path, demangle: bool) -> str:
    # llvm-nm demangles the kilobyte-long template names GNU nm gives up on. Demangled, the
    # whole table can be huge (a large sanitize image: 253 MB, names up to 2 MB).
    nm = tool("nm")
    return subprocess.run([nm, "-C" if demangle else "--no-demangle", "-S", "--defined-only",
                           str(elf)], capture_output=True, text=True).stdout


class Symbol(typing.NamedTuple):
    address: int
    size: int
    name: str


def function_table(mangled_nm: str) -> list[tuple[int, int, list[str]]]:
    """The ELF's functions as (start, size, mangled names), sorted by start. Several names at one
    start are one body the linker folded (--icf) - often unrelated functions that compiled alike.
    Mangled: demangling the whole table costs hundreds of MB; symbol_names() does the hit ones."""
    sized: dict[int, tuple[int, list[str]]] = {}
    # size 0: linker labels (`_LINKER_INTERN_*`), asm without .size
    labels: dict[int, list[str]] = {}
    for line in mangled_nm.splitlines():
        m = re.match(r"([0-9a-f]{8}) ([0-9a-f]{8}) [tTwW] (.+)$", line)
        if not m:
            continue
        start, size, name = int(m.group(1), 16), int(
            m.group(2), 16), m.group(3)
        if size == 0:
            labels.setdefault(start, []).append(name)
        else:
            old_size, names = sized.get(start, (0, []))
            sized[start] = (max(old_size, size), names + [name])
    # A label only names a body where no function of its own starts: next to one it is not a
    # second function folded into it (main's body shares its start with two linker labels).
    for start, names in labels.items():
        sized.setdefault(start, (0, names))
    return sorted((start, size, names) for start, (size, names) in sized.items())


class Image:
    """What the tree's ELF knows. Each tool runs at most once per Image, and nothing is read before it is asked
    for: the demangled table can be 253 MB on a large sanitize image, so it is built once, never twice; `profile`
    reads only the mangled one."""

    def __init__(self, elf: Path, run_nm: typing.Callable[[Path, bool], str] = nm_text):
        self.path = elf
        self._run_nm = run_nm

    @functools.cached_property
    def demangled_nm(self) -> str:
        return self._run_nm(self.path, True)

    @functools.cached_property
    def mangled_nm(self) -> str:
        return self._run_nm(self.path, False)

    @functools.cached_property
    def objects(self) -> list[Symbol]:
        """The data objects (nm types bBdDrRvV with a size), demangled."""
        table = []
        for line in self.demangled_nm.splitlines():
            m = re.match(r"([0-9a-f]{8}) ([0-9a-f]{8}) [bBdDrRvV] (.+)$", line)
            if m:
                table.append(Symbol(int(m.group(1), 16),
                             int(m.group(2), 16), m.group(3)))
        return table

    @functools.cached_property
    def functions(self) -> list[tuple[int, int, list[str]]]:
        return function_table(self.mangled_nm)

    @functools.cached_property
    def parsed(self) -> kvasir_elf.Elf:
        return kvasir_elf.Elf(self.path.read_bytes())

    def resident(self):
        """kvasir_elf's resident sections: runs where it is loaded, not writable."""
        return self.parsed.resident()

    @functools.cached_property
    def code_ranges(self) -> list[tuple[int, int]]:
        """[start, end) of every section that holds instructions (SHF_EXECINSTR), at its run address: what
        objdump -h calls CODE."""
        return [(s["addr"], s["addr"] + s["size"]) for s in self.parsed.sections
                if s["flags"] & SHF_EXECINSTR and s["size"]]

    def is_code(self, address: int) -> bool:
        return any(lo <= (address & ~1) < hi for lo, hi in self.code_ranges)

    def exact(self, name: str) -> Symbol | None:
        """The data object with exactly this demangled name."""
        return next((s for s in self.objects if s.name == name), None)

    def any_exact(self, name: str, types: str = "") -> Symbol | None:
        """Any symbol of the demangled table with exactly this name (of these nm types, if given), sized or not:
        linker symbols (`_LINKER_stack_start_`), functions."""
        for m in re.finditer(r"^([0-9a-f]{8}) (?:([0-9a-f]{8}) )?(\S) (.+)$", self.demangled_nm, re.M):
            if m.group(4) == name and (not types or m.group(3) in types):
                return Symbol(int(m.group(1), 16), int(m.group(2) or "0", 16), name)
        return None

    def one(self, pattern: str) -> Symbol:
        """The one data object the regex names: an exact name wins, else the only match; else the candidates."""
        rx = re.compile(pattern)
        hits = [s for s in self.objects if rx.search(s.name)]
        exact = [s for s in hits if s.name == pattern]
        hits = exact or hits
        if len(hits) != 1:
            shown = "\n  ".join(h.name[:140] for h in hits[:8])
            die(f"'{pattern}' matches {len(hits)} objects, it has to match one" +
                (f":\n  {shown}" if hits else ""))
        return hits[0]


def addr2line(tree, addresses: list[int]) -> dict[int, str]:
    """Address -> `function  file:line`, the paths shown as the tree shows them."""
    addr2line_tool = tool("addr2line")
    out = subprocess.run([addr2line_tool, "-f", "-C", "-e", str(tree.elf)] + [f"0x{a:x}" for a in addresses],
                         capture_output=True, text=True).stdout.splitlines()
    result = {}
    for i, a in enumerate(addresses):
        if 2 * i + 1 < len(out):
            fn = display_name(out[2 * i])
            where = tree.short_path(out[2 * i + 1])
            result[a] = f"{fn}  {where}"
    return result
