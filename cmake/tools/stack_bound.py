#!/usr/bin/env python3
"""Static worst-case stack bound of a linked Kvasir image, per core: the deepest main path, the interrupts by
priority level, the fault path on its own SP, against MIN_STACK_SIZE / CORE1_STACK_SIZE and the fault reserve.

    stack_bound.py <elf> [--notes <file>]... [--ci <file>]... [--txt <out>] [--json <out>] [--path] [--check]
                   [--quiet] [--fp] [--objdump <llvm-objdump>]

The user command is `tools/size_report.py stack` (the same arguments); util.cmake runs it in every image's post-link
(kvasir_postlink.py), writing <target>_stack.txt and _stack.json silently; STACK_BOUND_CHECK adds --check.

Inputs, all from the ELF and files next to it (nothing is built):
  frames      clang: the `.stack_sizes` section ([u32 address][ULEB128 bytes] records, lld --mllvm=-stack-size-section,
              non-allocated). gcc: the LTO link's `<elf>.ltrans<N>.ltrans.ci` (-fstack-usage -fcallgraph-info=su at link
              time), found next to the ELF unless --ci names them. Every function also gets a frame read from its code
              (the deepest running SP offset over its instructions: push/pop, vpush/vpop, sub/add sp, stmdb/ldmia sp!,
              str/ldr with sp writeback); the frame used is max(record, code). A function without a record (assembly,
              precompiled libraries) has only the code reading: an estimate, marked `~`.
  calls       llvm-objdump -d of every executable section at its run address: bl/blx <imm>, branches out of the
              function (tail calls), linker thunks/veneers by name, blx/bx rN with rN from a literal or movw/movt, the
              -fsanitize=function type hash (0xC105CAFE + hash before the call: every function with that prefix), the
              static-constructor loop over [_LINKER_init_array_start_, _end_). Anything else is an unresolved call (`!`).
  roots       core 0's vector table (the Startup's nvicIsrVectors symbol), core 1's ([_LINKER_INTERN_core1_vectors_*]).
  levels      `.kvasir.isr_levels` (StartUp.hpp, non-allocated): per core {core, n, (index, level) x n} int16, index =
              IRQ number or exception number - 16, level = the priority the Startup list leaves it at (-1: set at run
              time). Without it every handler counts as its own level ("all nest").
  notes       annotation files, merged (SDK, chip, firmware), one directive a line, `#` comments; names mangled or
              demangled, globs (fnmatch), no spaces (write `*`):
                frame <fn> <bytes>        the frame of a function the code reading cannot see
                calls <fn> <callee>...    the targets of <fn>'s indirect calls
                indirect <fn> <bytes>     <fn>'s indirect calls go to code we cannot see: budget them
                rom <fn>                  <fn>'s indirect calls go into the boot ROM: not measured, LOWER BOUND
                once <fn>                 an error path (it logs and stops: panic, UB report, assert): a path enters
                                          one error path at most, and a call into one inside it stops there
                fault <fn>                a handler that ends the run (an unhandled interrupt): in the fault class
                rom_region <lo> <hi>      the boot ROM's addresses: a call there is a boot-ROM call
                rom_pointer <addr>...     a 16-bit pointer to the ROM's function lookup is read from <addr>: a call
                                          through it, and through what it returns, is a boot-ROM call
                recurse <fn> <depth>      a bounded recursion through <fn>: the cycle counts <depth> times
                cut <caller> <callee>     an edge that cannot happen

The computation: strongly connected components (Tarjan), longest path over the condensed graph. A cycle counts each
member's frame once - `recurse` multiplies it - and is a `!` (recursion) unless `once` or `recurse` covers it. Per core,
bound = entry + sum over priority levels of the deepest handler at that level (+ the exception frame each: 32 B basic
frame + 4 B realignment, 104 + 4 with FP context when the image has an FP instruction; DDI0403E B1.5.6-B1.5.7). Only a
strictly higher priority preempts, tail-chaining adds no frame. A handler that sets its own SP (Fault::HandlerFor) adds
only its exception frame and its own frame to the interrupted stack; its path is checked against the room below that SP
(and the bound against its reserve: the faulting context's frame must survive). A bound with any `!` reachable is a
LOWER BOUND, and the report says why.

Exit codes: 0, 1 with --check when a bound exceeds its limit, 2 when the ELF cannot be read. Without --quiet it prints
the report; with --quiet only a failed check.
"""

import argparse
import bisect
import fnmatch
import json
import re
import shutil
import struct
import subprocess
import sys
from pathlib import Path

sys.dont_write_bytecode = True      # no __pycache__ in cmake/tools
sys.path.insert(0, str(Path(__file__).resolve().parent))
from kvasir_elf import SHF_ALLOC, SHN_UNDEF, Elf  # noqa: E402

SHF_EXECINSTR = 0x4
STT_NOTYPE, STT_OBJECT, STT_FUNC, STT_SECTION, STT_FILE = 0, 1, 2, 3, 4
# -fsanitize=function: [f-8], [f-4] = the function type's hash
SANITIZE_SIGNATURE = 0xC105CAFE
# R0-R3, R12, LR, PC, xPSR + the 8-byte realignment
BASIC_FRAME = 32 + 4
# + S0-S15, FPSCR, reserved (CONTROL.FPCA = 1)
EXTENDED_FRAME = 104 + 4
# NMI, HardFault, MemManage, BusFault, UsageFault, SecureFault
FAULT_VECTORS = range(2, 8)
VECTOR_NAMES = {2: 'NMI', 3: 'HardFault', 4: 'MemManage', 5: 'BusFault', 6: 'UsageFault', 7: 'SecureFault',
                11: 'SVCall', 12: 'DebugMonitor', 14: 'PendSV', 15: 'SysTick'}

CONDITIONS = 'eq|ne|cs|hs|cc|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le|al'
RX_INSN = re.compile(r'^\s*([0-9a-f]+):\s+(\S+)\s*(.*)$')
RX_B = re.compile(rf'^b({CONDITIONS})?(\.n|\.w)?$')
RX_BL = re.compile(rf'^bl({CONDITIONS})?$')
RX_BLX = re.compile(rf'^blx({CONDITIONS})?$')
RX_BX = re.compile(rf'^bx({CONDITIONS})?$')
RX_CB = re.compile(r'^cbn?z$')
RX_PUSH = re.compile(rf'^push({CONDITIONS})?(\.w)?$')
RX_POP = re.compile(rf'^pop({CONDITIONS})?(\.w)?$')
RX_SPADJ = re.compile(r'^(sub|add)(s|w)?(\.w|\.n)?$')
RX_TARGET = re.compile(r'0x([0-9a-f]+)')
RX_LITERAL = re.compile(r'@ 0x([0-9a-f]+)')
RX_THUNK = re.compile(r'^__\w*?(?:Thunk|thunk)_(.+)$|^__(.+)_veneer$')


def uleb(data, i):
    value = shift = 0
    while True:
        byte = data[i]
        i += 1
        value |= (byte & 0x7F) << shift
        shift += 7
        if byte < 0x80:
            return value, i


def parse_stack_sizes(blob):
    """A `.stack_sizes` section's bytes -> {address without the Thumb bit: bytes}."""
    out, i = {}, 0
    while i + 4 < len(blob) + 1 and i < len(blob):
        (addr,) = struct.unpack_from('<I', blob, i)
        size, i = uleb(blob, i + 4)
        out[addr & ~1] = size
    return out


RX_CI_NODE = re.compile(
    r'node:\s*\{\s*title:\s*"([^"]*)"\s*label:\s*"([^"]*)"')
RX_CI_BYTES = re.compile(r'\\n(\d+) bytes \(([a-z,]+)\)')


def parse_ci(text):
    """gcc -fcallgraph-info=su text -> {mangled name: (bytes, qualifier)}; a title is `<obj>:<mangled>` or `<mangled>`."""
    out = {}
    for m in RX_CI_NODE.finditer(text):
        title, label = m.group(1), m.group(2)
        b = RX_CI_BYTES.search(label)
        if not b:
            continue
        name = title.rsplit(':', 1)[-1]
        out[name] = (int(b.group(1)), b.group(2))
    return out


CALLER_SAVED = ('r0', 'r1', 'r2', 'r3', 'r12', 'ip', 'lr')


def clobber(regs):
    for r in CALLER_SAVED:
        regs.pop(r, None)


def register_list(args):
    """`{r4, r5, lr}` / `{d8-d11}` -> the registers named."""
    if '{' not in args:
        return []
    inner = args[args.index('{') + 1:args.index('}')
                 if '}' in args else len(args)]
    regs = []
    for part in inner.split(','):
        part = part.strip()
        m = re.match(r'^([rsdq])(\d+)-[rsdq]?(\d+)$', part)
        if m:
            regs += [f'{m.group(1)}{n}' for n in range(int(m.group(2)),
                                                       int(m.group(3)) + 1)]
        elif part:
            regs.append(part)
    return regs


def arm_attributes(elf):
    """The `aeabi` file attributes of .ARM.attributes: {tag: value} (Tag_CPU_name 5 a string, Tag_FP_arch 10 a
    number; ARM IHI 0045 "Addenda to the ABI for the Arm architecture", the build attributes). {} without the section."""
    sec = elf.section('.ARM.attributes')
    if sec is None:
        return {}
    data = bytes(elf.data[sec['offset']:sec['offset'] + sec['size']])
    out = {}
    if not data or data[0] != ord('A'):
        return out
    i = 1
    while i + 4 <= len(data):
        (length,) = struct.unpack_from('<I', data, i)
        end = i + length
        vendor_end = data.index(0, i + 4)
        vendor = data[i + 4:vendor_end]
        j = vendor_end + 1
        while vendor == b'aeabi' and j + 5 <= end:
            tag, (size,) = data[j], struct.unpack_from('<I', data, j + 1)
            k, sub_end = j + 5, j + size
            while tag == 1 and k < sub_end:      # Tag_File: the attributes of the whole file
                attr, k = uleb(data, k)
                if attr in (4, 5, 67) or (attr > 32 and attr % 2 == 1):
                    z = data.index(0, k)
                    out[attr] = data[k:z].decode('ascii', 'replace')
                    k = z + 1
                elif attr == 32:
                    _, k = uleb(data, k)
                    k = data.index(0, k) + 1
                else:
                    out[attr], k = uleb(data, k)
            j = sub_end
        i = end if length else len(data)
    return out


# a CPU of each Tag_CPU_arch (6) for llvm-objdump's decoder, when Tag_CPU_name is not one it knows (gcc writes the
# architecture there: `6S-M`, `8-M.MAIN`): v6-M, v6S-M, v7E-M, v8-M.baseline, v8-M.mainline (ARM IHI 0045, Tag_CPU_arch)
DECODER_CPU = {11: 'cortex-m0plus', 12: 'cortex-m0plus',
               13: 'cortex-m4', 16: 'cortex-m23', 17: 'cortex-m33'}


def disassembly_cpu(attributes):
    name = attributes.get(5)
    if isinstance(name, str) and name.lower().startswith('cortex-'):
        return name.lower()
    return DECODER_CPU.get(attributes.get(6))


def demangle(names):
    names = list(names)
    for tool in ('llvm-cxxfilt', 'c++filt', 'arm-none-eabi-c++filt'):
        if shutil.which(tool):
            out = subprocess.run([tool], input='\n'.join(names) + '\n', capture_output=True, text=True,
                                 errors='replace').stdout.splitlines()
            if len(out) == len(names):
                return dict(zip(names, out))
    return {n: n for n in names}


def plain(name):
    """A demangled name without its return type, template arguments and parameters, for matching notes."""
    depth, out = 0, []
    for c in name:
        if c in '<(':
            depth += 1
        elif c in '>)':
            depth = max(0, depth - 1)
        elif depth == 0:
            out.append(c)
    s = ''.join(out).strip()
    return s.rsplit(' ', 1)[-1] if ' ' in s else s


class Notes:
    def __init__(self):
        self.frame, self.calls, self.indirect, self.rom = [], [], [], []
        self.once, self.recurse, self.cut, self.fault = [], [], [], []
        self.rom_pointer, self.rom_region = [], []
        self.sources = []

    def read(self, path):
        self.sources.append(str(path))
        for number, raw in enumerate(Path(path).read_text().splitlines(), 1):
            words = raw.split('#', 1)[0].split()
            if not words:
                continue
            kind, args = words[0], words[1:]
            where = f'{path}:{number}'
            arity = {'frame': 2, 'indirect': 2, 'rom': 1,
                     'once': 1, 'recurse': 2, 'cut': 2, 'fault': 1}
            if kind == 'calls' and len(args) >= 2:
                self.calls.append((args[0], args[1:]))
            elif kind == 'rom_pointer' and args:
                self.rom_pointer += [int(a, 0) for a in args]
            elif kind == 'rom_region' and len(args) == 2:
                self.rom_region.append((int(args[0], 0), int(args[1], 0)))
            elif kind in arity and len(args) == arity[kind]:
                if kind in ('frame', 'indirect', 'recurse'):
                    if not args[1].isdigit():
                        raise ValueError(
                            f'{where}: {kind} wants a number, not {args[1]!r}')
                    getattr(self, kind).append((args[0], int(args[1])))
                elif kind == 'cut':
                    self.cut.append((args[0], args[1]))
                else:
                    getattr(self, kind).append(args[0])
            else:
                raise ValueError(
                    f'{where}: not a stack-bound note: {raw.strip()!r}')


class Function:
    __slots__ = ('addr', 'size', 'names', 'name', 'keys', 'record', 'source', 'code', 'calls', 'unknown', 'flags',
                 'literals', 'derived', 'param_calls', 'rom_calls')

    def __init__(self, addr, size, names):
        self.addr, self.size, self.names = addr, size, names
        self.name = names[0]
        self.keys = list(names)
        self.record = None          # bytes from .stack_sizes / .ci
        self.source = None          # 'su', 'ci', 'note', 'thunk'
        self.code = 0               # bytes read from the code
        self.calls = set()          # callee nodes (addresses, or ('?', address))
        self.unknown = []           # unresolved indirect calls
        self.flags = set()          # 'sets sp', 'dynamic sp', 'typed:N', 'init_array:N', 'nosize'
        # every literal value loaded (the fault handler's reserve)
        self.literals = []
        self.derived = False        # no FUNC symbol: a label of assembly without .type
        # bx/blx rN on a register the caller set (the machine outliner's tail calls)
        self.param_calls = []
        # calls into the boot ROM (not measured: a LOWER BOUND)
        self.rom_calls = 0

    @property
    def frame(self):
        if self.source in ('note', 'thunk'):
            return self.record
        if self.record is None:
            return self.code
        return max(self.record, self.code)


class Image:
    """One ELF's functions, call graph and roots."""

    def __init__(self, path, elf=None, objdump=None, ci_files=(), notes=None, fp=None):
        self.path = Path(path)
        self.elf = elf if elf is not None else Elf(
            self.path.read_bytes())   # the post-link driver's own parse
        self.notes = notes or Notes()
        self.objdump = objdump or self._find_objdump()
        self.messages = []
        self._functions()
        self._records(ci_files)
        self._disassemble()
        self._thunks()
        self._apply_notes()
        # FP context in the exception frames: an FP instruction seen, or (conservatively) undecoded words inside
        # functions of an image whose attributes say it has an FPU (Tag_FP_arch, 10)
        self.fp_undecoded = not self.fp_seen and self.undecoded > 0 and bool(
            self.attributes.get(10))
        self.fp_used = (
            self.fp_seen or self.fp_undecoded) if fp is None else fp
        self.exception_frame = EXTENDED_FRAME if self.fp_used else BASIC_FRAME

    # --- inputs ---

    @staticmethod
    def _find_objdump():
        for tool in ('llvm-objdump',):
            if shutil.which(tool):
                return tool
        raise FileNotFoundError(
            'no llvm-objdump in PATH (the stack bound reads its disassembly)')

    def sym(self, name):
        found = self.elf.symbol(name)
        return found[0] if found else None

    def word(self, addr):
        raw = self.elf.bytes_at(addr, 4)
        return struct.unpack('<I', raw)[0] if raw is not None and len(raw) == 4 else None

    def _functions(self):
        elf = self.elf
        code_sections = [i for i, s in enumerate(elf.sections) if s['flags'] & SHF_ALLOC and
                         s['flags'] & SHF_EXECINSTR and s['size']]
        self.code_ranges = [(elf.sections[i]['addr'], elf.sections[i]['addr'] + elf.sections[i]['size'])
                            for i in code_sections]
        by_addr = {}
        section_end = {}
        for s in elf.symbols:
            if not s['name'] or s['shndx'] == SHN_UNDEF or s['shndx'] not in code_sections:
                continue
            if s['type'] == STT_FUNC:
                addr = s['value'] & ~1
                entry = by_addr.setdefault(addr, [0, []])
                entry[0] = max(entry[0], s['size'])
                # a global name first, a C one before a C++ one: aliases at one address (ICF, memset's, the reset
                # entry's .thumb_set name) share one record
                rank = (0 if s['bind'] else 2) + \
                    (1 if s['name'].startswith('_Z') else 0)
                entry[1].append((rank, s['name']))
                sec = elf.sections[s['shndx']]
                section_end[addr] = sec['addr'] + sec['size']
        for entry in by_addr.values():
            entry[1] = [n for _, n in sorted(entry[1])]
        sized = sorted((a, a + e[0]) for a, e in by_addr.items() if e[0])
        starts = [a for a, _ in sized]

        def inside_function(addr):
            i = bisect.bisect_right(starts, addr) - 1
            return i >= 0 and sized[i][0] <= addr < sized[i][1]
        # global labels of assembly without .type, where no FUNC covers them: a function start. Local labels are
        # branch targets inside a routine (RP2040 divider.S: FUNC entries of size 0, `y64_x32`, `last0` inside).
        derived = {}
        for s in elf.symbols:
            if s['type'] != STT_NOTYPE or s['shndx'] not in code_sections or not s['name'] or not s['bind']:
                continue
            name = s['name']
            if name.startswith(('$', '.', '_LINKER', '__start_', '__stop_')):
                continue
            addr = s['value'] & ~1
            if addr in by_addr or inside_function(addr):
                continue
            derived.setdefault(addr, []).append(name)
            sec = elf.sections[s['shndx']]
            section_end[addr] = sec['addr'] + sec['size']
        for addr, names in derived.items():
            by_addr[addr] = [0, names]
        # assembly without .size: up to the next function start, or its section's end
        all_starts = sorted(by_addr)
        for addr, entry in by_addr.items():
            if entry[0] == 0:
                i = bisect.bisect_right(all_starts, addr)
                nxt = all_starts[i] if i < len(
                    all_starts) else section_end[addr]
                entry[0] = min(nxt, section_end[addr]) - addr or 2
        self.funcs = {}
        for addr, (size, names) in by_addr.items():
            f = Function(addr, size, names)
            f.derived = addr in derived
            if f.derived:
                f.flags.add('nosize')
            self.funcs[addr] = f
        self.starts = sorted(self.funcs)
        pretty = demangle({n for f in self.funcs.values() for n in f.names})
        for f in self.funcs.values():
            f.name = pretty.get(f.names[0], f.names[0])
            for n in f.names:
                p = pretty.get(n, n)
                f.keys += [p, plain(p)]
        self.byname = {}
        for f in self.funcs.values():
            for n in f.names:
                self.byname.setdefault(n, f.addr)

    def owner(self, addr):
        i = bisect.bisect_right(self.starts, addr) - 1
        if i < 0:
            return None
        f = self.funcs[self.starts[i]]
        return f.addr if addr < f.addr + max(f.size, 2) else None

    def _records(self, ci_files):
        self.record_kind = None
        for s in self.elf.sections:
            if s['name'] == '.stack_sizes':
                for addr, size in parse_stack_sizes(bytes(self.elf.data[s['offset']:s['offset'] + s['size']])).items():
                    if addr in self.funcs:
                        f = self.funcs[addr]
                        f.record, f.source = max(size, f.record or 0), 'su'
                self.record_kind = '.stack_sizes'
        if not ci_files:
            ci_files = sorted(self.path.parent.glob(
                self.path.name + '.ltrans*.ltrans.ci'))
        for ci in ci_files:
            self.record_kind = self.record_kind or '.ci'
            for name, (size, kind) in parse_ci(Path(ci).read_text(errors='replace')).items():
                addr = self.byname.get(name)
                if addr is None:
                    continue
                f = self.funcs[addr]
                if kind != 'static' and 'bounded' not in kind:
                    # gcc: `dynamic` alone is unbounded, `dynamic,bounded` is in N
                    f.flags.add('dynamic sp')
                f.record, f.source = max(size, f.record or 0), 'ci'

    def _disassemble(self):
        """One pass over llvm-objdump's text, line by line: frames from the code, calls, literals."""
        # address-taken functions: a Thumb address of one as a word anywhere the image loads (literal pools, tables,
        # data). An indirect call reaches only those - a direct `bl` target alone (the reset body) is no candidate.
        taken = set()
        for sec in self.elf.sections:
            if sec['flags'] & SHF_ALLOC and sec['type'] != 8 and sec['size'] >= 4:
                blob = bytes(
                    self.elf.data[sec['offset']:sec['offset'] + sec['size'] // 4 * 4])
                taken |= {w & ~1 for (w,) in struct.iter_unpack(
                    '<I', blob) if w & 1}
        self.address_taken = taken & set(self.funcs)
        typed = {}
        for f in self.funcs.values():
            if self.word(f.addr - 8) == SANITIZE_SIGNATURE and 'Thunk' not in f.names[0] and f.addr in taken:
                typed.setdefault(self.word(f.addr - 4), set()).add(f.addr)
        init = (self.sym('_LINKER_init_array_start_'),
                self.sym('_LINKER_init_array_end_'))
        self.init_array = []
        if None not in init:
            self.init_array = [(self.word(a) or 0) & ~
                               1 for a in range(init[0], init[1], 4)]
        self.fp_seen = False
        self.callsites = {}
        self.pending_returns = []
        # calls that do not return: the error paths (`once`) and the C library's assert/abort
        self.noreturn = {
            f.addr for p in self.notes.once for f in self.matching(p)}
        self.noreturn |= {self.byname[n] for n in ('__assert_func', '__assert_fail', 'abort', '_exit', 'exit', '_Exit')
                          if n in self.byname}
        self.attributes = arm_attributes(self.elf)
        self.undecoded = 0
        # the image's own CPU: without it llvm-objdump leaves the M33's FP and coprocessor instructions undecoded
        cpu = [f'--mcpu={c}'] if (c :=
                                  disassembly_cpu(self.attributes)) else []
        cmd = [self.objdump, '-d', '--no-show-raw-insn',
               '--print-imm-hex', *cpu, str(self.path)]
        with subprocess.Popen(cmd, stdout=subprocess.PIPE, text=True, errors='replace') as proc:
            state = None
            for line in proc.stdout:
                m = RX_INSN.match(line)
                if not m:
                    continue
                pc = int(m.group(1), 16)
                cur = self.owner(pc)
                if cur is None:
                    state = None
                    continue
                if state is None or state[0] != cur:
                    # function, SP offset, registers, literals since a call, SP copies, registers written,
                    # register states at forward branch targets, "the instruction before does not fall through"
                    state = [cur, 0, {}, [], set(), set(), {}, [False]]
                self._instruction(self.funcs[cur], state, pc, m.group(
                    2), m.group(3), typed, init)
        if proc.returncode:
            raise RuntimeError(
                f'{" ".join(cmd)} exited with {proc.returncode}')
        self._resolve_param_calls()
        for f, callee, site in self.pending_returns:
            # a call through what <callee> returned: the boot ROM's function when <callee> (or one it calls) looked it up
            g = self.funcs[callee]
            if 'rom lookup' in g.flags or any(isinstance(c, int) and 'rom lookup' in self.funcs[c].flags for c in g.calls):
                f.rom_calls += 1
            else:
                f.unknown.append(site + ' (what a call returned)')

    def _resolve_param_calls(self):
        """An outlined function's `bx rN` on a register it never wrote is its caller's call: the outliner moved the
        tail of the caller's code, the jump through rN included. Each call site gets the edge to what the caller left
        in rN (the outlined function itself pushes nothing)."""
        for f in self.funcs.values():
            for op, r, pc in f.param_calls:
                sites = self.callsites.get(f.addr, [])
                if not sites:
                    f.unknown.append(
                        f'{op} {r} @{pc:#x} (a register its caller sets)')
                for caller, snap in sites:
                    v = snap.get(r)
                    if isinstance(v, int) and (v & ~1) in self.funcs:
                        self.funcs[caller].calls.add(v & ~1)
                    else:
                        self.funcs[caller].unknown.append(
                            f'{op} {r} @{pc:#x} in {f.names[0]}')

    def _instruction(self, f, state, pc, op, rest, typed, init):
        if op == '<unknown>':
            # an instruction llvm-objdump could not decode for this CPU
            self.undecoded += 1
            return
        if op.startswith('.') or op.startswith('<'):
            # .word / .short: data in the function (literal pool)
            return
        args = rest.split('@', 1)[0].strip()
        regs, lits, spregs, written, targets, dead = state[
            2], state[3], state[4], state[5], state[6], state[7]
        # the register values one linear pass can know: at a forward branch target the states of both ways meet (only
        # the branch's when the code before it does not fall through: a return, a jump, an error path)
        if pc in targets:
            snap = targets.pop(pc)
            if dead[0]:
                regs.clear()
                regs.update(snap)
            else:
                for k in [k for k in regs if regs[k] != snap.get(k)]:
                    del regs[k]
        elif dead[0]:
            # reached from somewhere this pass has not seen: nothing known
            regs.clear()
        dead[0] = False
        dest0 = args.split(',')[0].strip() if args else ''
        if re.match(r'^(r\d+|lr|ip)$', dest0) and not op.startswith(('str', 'cmp', 'cmn', 'tst', 'teq', 'push', 'stm',
                                                                     'it', 'b', 'cb', 'dmb', 'dsb', 'isb', 'nop', 'vst')):
            written.add(dest0)
        if RX_POP.match(op) or op.startswith('ldm'):
            written.update(r.strip() for r in register_list(args))
            if op.startswith('ldm'):
                for r in register_list(args):
                    regs.pop(r.strip(), None)
        if op.startswith('v'):
            # any VFP instruction: CONTROL.FPCA can be set
            self.fp_seen = True
        # --- the frame: the deepest running SP offset; a return does not unwind it (another path keeps the frame) ---
        regl = register_list(args)
        returns = False
        if RX_PUSH.match(op) or (op.startswith('stmdb') and args.startswith('sp!')):
            state[1] += 4 * len(regl)
        elif RX_POP.match(op) or (op.startswith('ldm') and args.startswith('sp!')):
            returns = 'pc' in regl
            if not returns:
                state[1] -= 4 * len(regl)
            elif regl:
                pass
            if regl and not returns:
                last = regl[-1].strip()
                regs[last] = 'popped'
        elif op in ('vpush', 'vpush.64', 'vpush.32'):
            state[1] += (8 if any(r.startswith('d')
                         for r in regl) else 4) * len(regl)
        elif op in ('vpop', 'vpop.64', 'vpop.32'):
            state[1] -= (8 if any(r.startswith('d')
                         for r in regl) else 4) * len(regl)
        elif RX_SPADJ.match(op) and re.match(r'^sp, (sp, )?#', args):
            imm = int(args.split('#')[1].split()[0].rstrip(','), 0)
            state[1] += imm if op.startswith('sub') else -imm
        elif RX_SPADJ.match(op) and re.match(r'^sp, (sp, )?r\d', args):
            value = regs.get(args.split(',')[-1].strip())
            if isinstance(value, int):
                # gcc on v6-M: ldr r4, =-N; add sp, r4
                delta = value - (1 << 32) if value >= 1 << 31 else value
                state[1] += delta if op.startswith('sub') else -delta
            else:
                f.flags.add('dynamic sp')
        elif op.startswith('str') and re.search(r'\[sp, #-0x([0-9a-f]+)\]!', args):
            state[1] += int(re.search(r'\[sp, #-0x([0-9a-f]+)\]!',
                            args).group(1), 16)
        elif op.startswith('ldr') and re.match(r'^(\w+), \[sp\], #0x([0-9a-f]+)', args):
            mm = re.match(r'^(\w+), \[sp\], #0x([0-9a-f]+)', args)
            if mm.group(1) == 'pc':
                returns = True
            else:
                state[1] -= int(mm.group(2), 16)
        f.code = max(f.code, state[1])
        # --- SP moved by a register ---
        if (op.startswith('mov') and args.startswith('sp, r')) or (op.startswith('msr') and re.match(r'^msp\b', args)):
            src = args.split(',')[1].strip() if ',' in args else ''
            if op.startswith('mov') and src in spregs:
                # a frame pointer put back (mov sp, r7)
                pass
            else:
                f.flags.add('sets sp')
        if (op.startswith('add') and re.match(r'^(r\d+), sp(, #|$)', args)) or \
                (op.startswith('mov') and re.match(r'^(r\d+), sp$', args)):
            spregs.add(args.split(',')[0])
            regs.pop(args.split(',')[0], None)
            return
        # --- calls ---
        target = RX_TARGET.search(args)
        if (RX_BL.match(op) or RX_BLX.match(op)) and target and not args.startswith('r') and not args.startswith('lr'):
            t = int(target.group(1), 16)
            if f.addr < (t & ~1) < f.addr + f.size:
                # gcc on v6-M: a `bl` as the far jump inside a function (b reaches +-2 KiB), not a call
                targets[t & ~1] = dict(regs) if (
                    t & ~1) > pc else targets.get(t & ~1, {})
                dead[0] = True
                return
            self._edge(f, t)
            callee = self.owner(t & ~1)
            if callee is not None and self.funcs[callee].names[0].startswith('OUTLINED_FUNCTION'):
                self.callsites.setdefault(callee, []).append(
                    (f.addr, {k: v for k, v in regs.items() if isinstance(v, int)}))
            clobber(regs)
            if callee is not None:
                # what it returns, should r0 be called next
                regs['r0'] = ('ret', callee)
            lits.clear()
            dead[0] = callee in self.noreturn
            return
        if (RX_B.match(op) or RX_CB.match(op)) and target:
            t = int(target.group(1), 16)
            if not (f.addr <= t < f.addr + f.size):
                # leaves the function: a tail call
                self._edge(f, t)
            elif t > pc:
                old = targets.get(t)
                targets[t] = dict(regs) if old is None else {
                    k: v for k, v in old.items() if regs.get(k) == v}
            dead[0] = op in ('b', 'b.n', 'b.w')
            return
        if op == 'bx' and args == 'lr' or returns and op in ('pop', 'pop.w', 'ldr', 'ldr.w', 'ldm', 'ldmia', 'ldmia.w'):
            dead[0] = True
            return
        if (RX_BLX.match(op) or RX_BX.match(op)) and args.split() and args.split()[0] != 'lr':
            r = args.split()[0]
            if r not in written and f.names[0].startswith('OUTLINED_FUNCTION'):
                f.param_calls.append((op, r, pc))
                return
            was = regs.get(r)
            self._indirect(f, op, r, pc, regs, lits, typed, init)
            if RX_BLX.match(op):
                clobber(regs)
                if was == 'romlookup':
                    # the ROM function the lookup returned
                    regs['r0'] = 'romptr'
                lits.clear()
            return
        if op.startswith('ldr') and args.startswith('pc'):
            lit = RX_LITERAL.search(rest)
            if returns:
                return
            if lit and '[pc' in args:
                value = self.word(int(lit.group(1), 16))
                if value is not None:
                    self._edge(f, value & ~1)
                    return
            f.unknown.append(f'{op} {args} @{pc:#x}')
            return
        if (op.startswith('mov') or op.startswith('add')) and args.startswith('pc'):
            return                                  # a jump table: stays in the function
        # --- register values: literals, movw/movt, copies ---
        dest = args.split(',')[0].strip() if args else ''
        mm = re.match(r'^(r\d+), \[(r\d+)\]$',
                      args) if op.startswith('ldrh') else None
        if mm and regs.get(mm.group(2)) in self.notes.rom_pointer:
            regs[mm.group(1)] = 'romlookup'
            return
        if op.startswith('ldr') and '[pc' in args:
            lit = RX_LITERAL.search(rest)
            value = self.word(int(lit.group(1), 16)) if lit else None
            regs[dest] = value
            if value is not None:
                lits.append(value)
                f.literals.append(value)
            return
        if op in ('movw', 'mov.w', 'movs', 'mov') and '#' in args and re.match(r'^r\d+|^lr|^ip', dest):
            try:
                regs[dest] = int(args.split('#')[1].split()[0], 0)
            except ValueError:
                regs.pop(dest, None)
            return
        if op == 'movt' and '#' in args:
            low = regs.get(dest)
            regs[dest] = ((low if isinstance(low, int) else 0) & 0xFFFF) | (
                int(args.split('#')[1].split()[0], 0) << 16)
            if isinstance(regs[dest], int):
                f.literals.append(regs[dest])
            return
        mm = re.match(r'^(r\d+), (r\d+), #(0x[0-9a-f]+|\d+)$',
                      args) if op in ('lsls', 'lsl', 'lsls.w') else None
        if mm and isinstance(regs.get(mm.group(2)), int):
            regs[dest] = (regs[mm.group(2)] << int(
                mm.group(3), 0)) & 0xFFFFFFFF
            return
        if op in ('mov', 'movs') and re.match(r'^(r\d+|lr|ip), (r\d+|lr|ip)$', args):
            src = args.split(',')[1].strip()
            regs[dest] = regs.get(src)
            if src in spregs:
                spregs.add(dest)
            return
        if re.match(r'^(r\d+|lr|ip)\b', dest) and not op.startswith(('str', 'cmp', 'cmn', 'tst', 'teq', 'push', 'stm',
                                                                     'it', 'dmb', 'dsb', 'isb', 'nop', 'cbz',
                                                                     'cbnz', 'b')):
            regs.pop(dest, None)
            spregs.discard(dest)

    def _edge(self, f, target):
        if self.in_rom(target):
            f.rom_calls += 1
            return
        o = self.owner(target & ~1)
        f.calls.add(o if o is not None else ('?', target & ~1))

    def _indirect(self, f, op, r, pc, regs, lits, typed, init):
        value = regs.get(r)
        if value == 'popped':
            # pop {rN}; bx rN: gcc's v6-M return
            return
        if isinstance(value, int) and (value & ~1) in self.funcs:
            f.calls.add(value & ~1)
            return
        if value in ('romlookup', 'romptr') or (isinstance(value, int) and self.in_rom(value)):
            f.rom_calls += 1
            if value == 'romlookup':
                f.flags.add('rom lookup')
            return
        if isinstance(value, tuple) and value[0] == 'ret':
            self.pending_returns.append((f, value[1], f'{op} {r} @{pc:#x}'))
            return
        if init[0] is not None and (init[0] in f.literals or init[1] in f.literals):
            f.calls |= {a for a in self.init_array if a in self.funcs}
            f.flags.add(f'init_array:{len(self.init_array)}')
            return
        if SANITIZE_SIGNATURE in lits:
            hashes = [x for x in lits if x in typed]
            if hashes:
                f.calls |= typed[hashes[-1]]
                f.flags.add(f'typed:{len(typed[hashes[-1]])}')
                return
        f.unknown.append(f'{op} {r} @{pc:#x}')

    def in_rom(self, addr):
        return any(lo <= addr < hi for lo, hi in self.notes.rom_region)

    def _in_code(self, addr):
        return any(lo <= addr < hi for lo, hi in self.code_ranges)

    def _thunks(self):
        for f in self.funcs.values():
            for n in f.names:
                m = RX_THUNK.match(n)
                if not m:
                    continue
                target = self.byname.get(m.group(1) or m.group(2))
                if target is not None:
                    f.calls, f.unknown, f.record, f.source = {
                        target}, [], 0, 'thunk'
                break

    def matching(self, pattern):
        return [f for f in self.funcs.values() if any(fnmatch.fnmatchcase(k, pattern) for k in f.keys)]

    def _apply_notes(self):
        n = self.notes
        self.extra_nodes = {}       # ('budget', fn address) -> bytes
        self.rom = set()
        for pattern, size in n.frame:
            for f in self.matching(pattern):
                f.record, f.source = size, 'note'
        for pattern, callees in n.calls:
            for f in self.matching(pattern):
                for c in callees:
                    f.calls |= {g.addr for g in self.matching(c)}
                f.unknown = []
        for pattern, size in n.indirect:
            for f in self.matching(pattern):
                if f.unknown:
                    self.extra_nodes[('budget', f.addr)] = size
                    f.calls.add(('budget', f.addr))
                    f.unknown = []
        for pattern in n.rom:
            for f in self.matching(pattern):
                if f.unknown or any(isinstance(c, tuple) and c[0] == '?' for c in f.calls):
                    self.rom.add(f.addr)
        for a, b in n.cut:
            drop = {g.addr for g in self.matching(b)}
            for f in self.matching(a):
                f.calls -= drop
        self.once = {f.addr for p in n.once for f in self.matching(p)}
        self.fault_class = {f.addr for p in n.fault for f in self.matching(p)}
        self.recurse = {}
        for p, depth in n.recurse:
            for f in self.matching(p):
                self.recurse[f.addr] = depth

    # --- the graph ---

    def frame(self, node):
        if isinstance(node, tuple):
            return self.extra_nodes.get(node, 0)
        return self.funcs[node].frame

    def label(self, node):
        if isinstance(node, tuple):
            if node[0] == 'budget':
                return f'<budgeted indirect calls of {short(self.funcs[node[1]].name)}>'
            return f'<{node[1]:#010x}>'
        return self.funcs[node].name

    def own_reasons(self, node):
        """The reasons a node alone gives: '!' makes a bound lower, '~' marks an estimate."""
        if isinstance(node, tuple):
            if node[0] == '?':
                if self._in_code(node[1]):
                    return {f'!call into {node[1]:#010x}: code no function symbol covers (assembly without a label)'}
                return {f'!call into {node[1]:#010x}: no code of the image there'}
            return {f'~{self.label(node)}: {self.extra_nodes[node]} B from the notes'}
        f = self.funcs[node]
        nm = short(f.name)
        out = set()
        if f.rom_calls:
            out.add(
                f'!{nm}: {f.rom_calls} call(s) into the boot ROM (not measured)')
        if f.unknown:
            if node in self.rom:
                out.add(
                    f'!{nm}: calls into the boot ROM ({len(f.unknown)} site(s), not measured)')
            else:
                out.add(f'!{nm}: indirect call {f.unknown[0]}' + (f' (+{len(f.unknown) - 1})' if len(f.unknown) > 1
                                                                  else ''))
        elif node in self.rom:
            out.add(f'!{nm}: calls into the boot ROM (not measured)')
        if 'dynamic sp' in f.flags:
            out.add(f'!{nm}: dynamic stack (sub sp, rN or a dynamic gcc frame)')
        return out

    def solve(self):
        """The deepest path from every node: self.depth[node], self.chain(node), self.reasons[node].

        A call into a `once` function (an error path: panic, UB report, canary, assert) is an error entry. A path takes
        at most one: D0 = the deepest path without one, D1 = with at most one, the error path itself being a D0 path
        (an error inside the error path stops there - it logs once and ends). The rest is Tarjan's SCCs over the graph
        without error entries and the longest path over the condensed graph; a cycle counts each member's frame once
        (`recurse`: <depth> times) and is a `!` unless `recurse` covers it."""
        once = self.once
        succ, errs = {}, {}

        def successors(v):
            if v not in succ:
                calls = sorted(self.funcs[v].calls, key=str) if not isinstance(
                    v, tuple) else []
                # a function that switches to a stack of its own (the reset entry, a fault handler) runs nothing on
                # its caller's: a call into it - a restart, or a type-hash candidate of a checked call - adds nothing
                calls = [w for w in calls if isinstance(
                    w, tuple) or 'sets sp' not in self.funcs[w].flags]
                succ[v] = [w for w in calls if w not in once]
                errs[v] = [w for w in calls if w in once]
            return succ[v]
        index, low, on, stack, sccs = {}, {}, set(), [], []
        counter = 0
        for root in list(self.funcs):
            if root in index:
                continue
            work = [(root, 0)]
            while work:
                v, i = work.pop()
                if i == 0:
                    index[v] = low[v] = counter
                    counter += 1
                    stack.append(v)
                    on.add(v)
                children = successors(v)
                if i < len(children):
                    work.append((v, i + 1))
                    w = children[i]
                    if w not in index:
                        work.append((w, 0))
                    elif w in on:
                        low[v] = min(low[v], index[w])
                    continue
                if low[v] == index[v]:
                    comp = []
                    while True:
                        w = stack.pop()
                        on.discard(w)
                        comp.append(w)
                        if w == v:
                            break
                    sccs.append(comp)
                if work:
                    parent = work[-1][0]
                    low[parent] = min(low[parent], low[v])
        self.scc = {}
        weight, own = {}, {}
        for comp in sccs:
            key = comp[0]
            members = set(comp)
            for v in comp:
                self.scc[v] = comp
            why = set()
            for v in comp:
                why |= self.own_reasons(v)
            w = sum(self.frame(v) for v in comp)
            if len(comp) > 1 or any(v in successors(v) for v in comp):
                names = ' -> '.join(short(self.label(v), 40)
                                    for v in comp[::-1][:6]) + (' ...' if len(comp) > 6 else '')
                depth = max(
                    (self.recurse[v] for v in comp if v in self.recurse), default=0)
                if depth:
                    w *= depth
                    why.add(
                        f'~recursion counted {depth} times (notes): {names}')
                else:
                    why.add(f'!recursion: {names}')
            weight[key], own[key] = w, why
            del members
        d0, d1, best0, best1, r0, r1 = {}, {}, {}, {}, {}, {}
        for comp in sccs:                           # children first, in the graph without error entries
            key = comp[0]
            members = set(comp)
            outs = [w for v in comp for w in successors(v) if w not in members]
            nxt = max(outs, key=lambda w: d0[self.scc[w][0]], default=None)
            d0[key] = weight[key] + \
                (d0[self.scc[nxt][0]] if nxt is not None else 0)
            best0[key] = nxt
            why = set(own[key])
            for w in outs:
                why |= r0[self.scc[w][0]]
            r0[key] = frozenset(why)
        for comp in sccs:
            key = comp[0]
            members = set(comp)
            outs = [(w, 1)
                    for v in comp for w in successors(v) if w not in members]
            outs += [(e, 0) for v in comp for e in errs[v]]

            def value(choice):
                w, mode = choice
                return (d1 if mode else d0)[self.scc[w][0]]
            nxt = max(outs, key=value, default=None)
            d1[key] = weight[key] + (value(nxt) if nxt is not None else 0)
            best1[key] = nxt
            why = set(own[key])
            for w, mode in outs:
                why |= (r1 if mode else r0)[self.scc[w][0]]
            r1[key] = frozenset(why)
        self._best0, self._best1 = best0, best1
        self.depth = {v: d1[self.scc[v][0]] for v in self.scc}
        self.reasons = {v: r1[self.scc[v][0]] for v in self.scc}

    def chain(self, node):
        """[(bytes, name)] along the deepest path from node; a cycle is one entry of its members."""
        out, mode, seen = [], 1, set()
        while node is not None and node not in seen:
            comp = self.scc[node]
            seen |= set(comp)
            if len(comp) > 1:
                out.append((sum(self.frame(v) for v in comp),
                            ' | '.join(short(self.label(v), 50) for v in comp) + ' (cycle)'))
            else:
                out.append((self.frame(node), self.label(node)))
            if mode:
                nxt = self._best1[comp[0]]
                node, mode = (nxt if nxt is not None else (None, 0))
            else:
                node = self._best0[comp[0]]
        return out


def short(name, width=90):
    return name if len(name) <= width else name[:width - 3] + '...'


def lower(reasons):
    return any(r.startswith('!') for r in reasons)


def read_levels(elf):
    """`.kvasir.isr_levels` -> {core: {index: level}}; None without the section."""
    sec = elf.section('.kvasir.isr_levels')
    if sec is None:
        return None
    blob = bytes(elf.data[sec['offset']:sec['offset'] + sec['size']])
    values = [v for (v,) in struct.iter_unpack(
        '<h', blob[:len(blob) // 2 * 2])]
    out, i = {}, 0
    while i + 1 < len(values):
        core, n = values[i], values[i + 1]
        pairs = values[i + 2:i + 2 + 2 * n]
        out.setdefault(core, {}).update(
            {pairs[k]: pairs[k + 1] for k in range(0, len(pairs) - 1, 2)})
        i += 2 + 2 * n
    return out


def vector_table(image, core):
    """(address, word count) of a core's vector table, or None."""
    elf = image.elf
    if core == 0:
        start = image.sym('_LINKER_vectors_start_')
        for s in elf.symbols:
            if s['name'].endswith('14nvicIsrVectorsE') and s['value'] == start and s['size']:
                return start, s['size'] // 4
        after = image.sym('_LINKER_INTERN_after_vectors_start_')
        sec = elf.section('.vectors')
        if start is None and sec is not None:
            start = sec['addr']
        if start is None:
            return None
        end = after if after and after > start else (
            sec['addr'] + sec['size'] if sec else start)
        return start, (end - start) // 4
    start, end = image.sym('_LINKER_INTERN_core1_vectors_start_'), image.sym(
        '_LINKER_INTERN_core1_vectors_end_')
    if start is None or end is None or end <= start:
        return None
    return start, (end - start) // 4


def fault_room(image, handler, top, bottom, protector=0):
    """The reserve a Fault::HandlerFor leaves on top of the stack and the room below its SP, from the handler's literal
    pool (top - reserve, top, bottom): SP = max(top - reserve, (top + bottom) / 2 rounded down to 8)
    (KVASIR_FAULT_SAFE_SP). None when the pool does not hold both ends of this stack."""
    lits = set(image.funcs[handler].literals)
    if top is None or bottom is None or top not in lits or bottom not in lits:
        return None
    below = [v for v in lits if v < top and v != bottom and (
        top - v) % 8 == 0 and top - v <= 0x100000]
    middle = ((top + bottom) >> 4) << 3
    reserve = top - max(below) if below else None
    sp = max(top - reserve, middle) if reserve is not None else middle
    return {'reserve': reserve, 'effective_reserve': top - sp, 'room': sp - bottom - protector}


def analyse(image):
    image.solve()
    levels = read_levels(image.elf)
    out = {'image': image.path.name, 'exception_frame': image.exception_frame, 'fp_instructions': image.fp_used,
           'functions': len(image.funcs), 'records': image.record_kind,
           'with_record': sum(1 for f in image.funcs.values() if f.source in ('su', 'ci')),
           'from_code': sorted(short(f.name, 60) for f in image.funcs.values() if f.record is None),
           'level_table': levels is not None, 'notes': image.notes.sources, 'cores': [],
           'fp_from_undecoded': image.fp_undecoded}
    frame = image.exception_frame
    protector = image.sym('_LINKER_INTERN_stackProtector_size_') or 0
    for core in (0, 1):
        table = vector_table(image, core)
        if table is None or table[1] < 2:
            continue
        vaddr, count = table
        words = [image.word(vaddr + 4 * i) or 0 for i in range(count)]
        entry = words[1] & ~1
        if entry not in image.funcs:
            continue
        if core == 0:
            lo, hi = image.sym('_LINKER_stack_start_'), image.sym(
                '_LINKER_stack_end_')
            min_stack = image.sym('cmake_min_stack_size')
            limit = {'name': 'MIN_STACK_SIZE', 'bytes': (min_stack - protector) if min_stack is not None else None,
                     'text': f'MIN_STACK_SIZE {min_stack} B - {protector} B protector' if min_stack is not None
                     else 'no MIN_STACK_SIZE symbol'}
        else:
            lo, hi = image.sym('_LINKER_stack1_start_'), image.sym(
                '_LINKER_stack1_end_')
            limit = {'name': 'CORE1_STACK_SIZE', 'bytes': (hi - lo) if None not in (lo, hi) else None,
                     'text': f'CORE1_STACK_SIZE {hi - lo} B' if None not in (lo, hi) else 'no core 1 stack symbols'}
        e_depth, e_why = image.depth[entry], image.reasons[entry]
        handlers = {}
        for v, word in enumerate(words[2:], 2):
            h = word & ~1
            if word and h in image.funcs and h != entry:
                handlers.setdefault(h, []).append(v)
        core_levels = (levels or {}).get(core, {})
        groups, fault = {}, None
        for h, vectors in handlers.items():
            d = image.depth[h] + frame
            f = image.funcs[h]
            if 'sets sp' in f.flags:
                room = fault_room(image, h, hi, lo,
                                  protector if core == 0 else 0)
                fault = {'handler': f.name, 'vectors': vectors, 'depth': image.depth[h], 'on_stack': frame + f.frame,
                         'room': room, 'lower': lower(image.reasons[h]), 'reasons': sorted(image.reasons[h]),
                         'path': image.chain(h)}
                d, h_node = frame + f.frame, None     # its path runs below its own SP
            else:
                h_node = h
            known = [core_levels[v - 16]
                     for v in vectors if (v - 16) in core_levels]
            if set(vectors) & set(FAULT_VECTORS) or h in image.fault_class:
                # NMI, a fault on this stack, an unhandled interrupt: they end the run, one of them counts
                key, label = ('fault',), 'fault class (once)'
            elif levels is not None and known and all(lv >= 0 for lv in known):
                key, label = ('level', min(known)), f'level {min(known)}'
            else:
                key = ('own', h)
                label = (
                    'level unknown' if known else 'not in the level table') if levels is not None else 'own level'
            g = groups.get(key)
            if g is None or d > g['depth']:
                groups[key] = {'level': label, 'handler': f.name, 'vectors': sorted(set(vectors) | set(
                    g['vectors'] if g and key == ('fault',) else [])), 'depth': d, 'node': h_node}
            elif key == ('fault',):
                g['vectors'] = sorted(set(g['vectors']) | set(vectors))
        bound = e_depth + sum(g['depth'] for g in groups.values())
        why = set(e_why)
        for g in groups.values():
            if g['node'] is not None:
                why |= image.reasons[g['node']]
        one_level = e_depth + max((g['depth']
                                  for g in groups.values()), default=0)
        result = {'core': core, 'entry': {'name': image.funcs[entry].name, 'depth': e_depth,
                                          'lower': lower(e_why), 'path': image.chain(entry)},
                  'levels': [{k: v for k, v in g.items() if k != 'node'} for g in
                             sorted(groups.values(), key=lambda g: -g['depth'])],
                  'bound': bound, 'one_level': one_level, 'lower': lower(why), 'reasons': sorted(why),
                  'limit': limit, 'linked_stack': (hi - lo) if None not in (lo, hi) else None, 'fault': fault,
                  'failures': []}
        if limit['bytes'] is not None and bound > limit['bytes']:
            result['failures'].append(
                f'core {core}: bound {bound} B > {limit["text"]} = {limit["bytes"]} B')
        if fault and fault['room']:
            room = fault['room']
            if fault['depth'] > room['room']:
                result['failures'].append(f'core {core}: fault path {fault["depth"]} B > the {room["room"]} B below '
                                          f'the fault handler\'s SP')
            live = bound - sum(g['depth']
                               for k, g in groups.items() if k == ('fault',))
            if live > room['effective_reserve']:
                result['failures'].append(f'core {core}: bound without the fault class {live} B > the fault reserve '
                                          f'{room["effective_reserve"]} B (a fault would overwrite the live frames)')
        out['cores'].append(result)
    return out


def text_report(r, show_path=False):
    lines = [f'{r["image"]}: {r["functions"]} functions, {r["with_record"]} frames from '
             f'{r["records"] or "no records"}, {len(r["from_code"])} read from the code'
             + (f' ({", ".join(r["from_code"][:4])}{", ..." if len(r["from_code"]) > 4 else ""})'
                if r['from_code'] else ''),
             f'exception frame {r["exception_frame"]} B ('
             + ('FP context: undecoded instructions in an image with an FPU' if r.get('fp_from_undecoded')
                else 'FP context: the image has FP instructions' if r['fp_instructions'] else 'basic: no FP instruction')
             + '); ' + ('levels from .kvasir.isr_levels' if r['level_table']
                        else 'no .kvasir.isr_levels: every handler is its own level')]
    for c in r['cores']:
        lb = ' LOWER BOUND' if c['lower'] else ''
        lines.append(f'core {c["core"]}  entry {short(c["entry"]["name"], 60):<60} {c["entry"]["depth"]:7} B'
                     + (' LOWER BOUND' if c['entry']['lower'] else ''))
        for g in c['levels']:
            lines.append(f'        {g["level"]:<22} {short(g["handler"], 45):<45} + {g["depth"]:5} B'
                         f'  (vectors {",".join(map(str, g["vectors"][:6]))}{"..." if len(g["vectors"]) > 6 else ""})')
        # the limit on this line; the fault reserve's verdict is the ERROR lines below
        verdict = 'OVER' if c['limit']['bytes'] is not None and c['bound'] > c['limit']['bytes'] else 'ok'
        limit = c['limit']
        lines.append(f'        bound {c["bound"]:>67} B{lb}   {limit["text"]}: {verdict}'
                     + (f'; linked stack {c["linked_stack"]} B' if c['linked_stack'] else ''))
        if c['fault']:
            fl = c['fault']
            room = fl['room']
            lines.append(f'        fault {short(fl["handler"], 50)} (own SP) {fl["depth"]} B'
                         + (' LOWER BOUND' if fl['lower'] else '')
                         + (f'; room below its SP {room["room"]} B, reserve {room["effective_reserve"]} B'
                            if room else '') + f'; {fl["on_stack"]} B on the interrupted stack')
        for f in c['failures']:
            lines.append(f'        ERROR {f}')
        bang = [w for w in c['reasons'] if w.startswith('!')]
        for w in bang[:12]:
            lines.append(f'        {w}')
        if len(bang) > 12:
            lines.append(f'        ... {len(bang) - 12} more')
        path = c['entry']['path']
        chain = ' > '.join(f'{short(n, 50)} {b}' for b, n in path)
        lines.append(
            f'deepest path, core {c["core"]}: ' + (chain if show_path else short(chain, 400)))
        if show_path and c['fault']:
            lines.append(f'fault path, core {c["core"]}: '
                         + ' > '.join(f'{short(n, 50)} {b}' for b, n in c['fault']['path']))
    return '\n'.join(lines) + '\n'


def main(argv=None):
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('elf', type=Path)
    ap.add_argument('--notes', action='append', default=[], type=Path)
    ap.add_argument('--ci', action='append', default=[], type=Path, help="gcc: the link's *.ltrans*.ci (default: next "
                    "to the ELF)")
    ap.add_argument('--txt', type=Path)
    ap.add_argument('--json', type=Path)
    ap.add_argument('--path', action='store_true',
                    help='print the whole deepest chain')
    ap.add_argument('--check', action='store_true',
                    help='exit 1 when a bound is above its limit')
    ap.add_argument('--quiet', action='store_true',
                    help='print nothing unless --check fails')
    ap.add_argument('--fp', action='store_true',
                    help='count the FP context in every exception frame')
    ap.add_argument('--objdump')
    ap.add_argument('--no-default-notes', action='store_true',
                    help=f'leave out {DEFAULT_NOTES.name} (the SDK\'s)')
    args = ap.parse_args(argv)
    return run(args)


DEFAULT_NOTES = Path(__file__).resolve().parent / 'stack_bound_default.txt'


def run(args, elf=None):
    notes = Notes()
    try:
        tool = args.objdump or Image._find_objdump()
    except FileNotFoundError as e:
        # no llvm-objdump on this machine: no report, one line in its place - not a link failure unless asked to check
        if args.txt:
            args.txt.write_text(f'{args.elf.name}: no stack bound: {e}\n')
        if args.check:
            print(
                f'stack_bound: {args.elf.name}: STACK_BOUND_CHECK needs {e}', file=sys.stderr)
            return 1
        return 0
    try:
        for n in ([] if getattr(args, 'no_default_notes', False) else [DEFAULT_NOTES]) + list(args.notes):
            notes.read(n)
        image = Image(args.elf, elf=elf, objdump=tool, ci_files=args.ci, notes=notes,
                      fp=True if args.fp else None)
    except (OSError, ValueError, RuntimeError) as e:
        print(f'stack_bound: {args.elf}: {e}', file=sys.stderr)
        return 2
    report = analyse(image)
    text = text_report(report, args.path)
    if args.txt:
        args.txt.write_text(text)
    if args.json:
        args.json.write_text(json.dumps(report, indent=1) + '\n')
    failures = [f for c in report['cores'] for f in c['failures']]
    if not args.quiet:
        sys.stdout.write(text)
    if args.check and failures:
        if args.quiet:
            sys.stderr.write(text)
        print(f'stack_bound: {args.elf.name}: STACK_BOUND_CHECK failed: ' +
              '; '.join(failures), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
