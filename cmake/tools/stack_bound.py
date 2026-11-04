#!/usr/bin/env python3
"""Static worst-case stack bound of a linked Kvasir image, per core: the deepest main path, the interrupts by
priority level, the fault path on its own SP, against MIN_STACK_SIZE / CORE1_STACK_SIZE and the fault reserve.

    stack_bound.py <elf> [--notes <file>]... [--ci <file>]... [--txt <out>] [--json <out>] [--path] [--check]
                   [--exact] [--quiet] [--fp] [--objdump <llvm-objdump>]

The user command is `tools/size_report.py stack` (the same arguments); util.cmake runs it in every image's post-link
(kvasir_postlink.py), writing <target>_stack.txt and _stack.json silently; STACK_BOUND_CHECK adds --check,
STACK_BOUND_EXACT --exact (a LOWER BOUND fails as well).

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
              One linear pass per function: register values meet at forward branch targets; a callee-saved register
              the function writes exactly once keeps that value everywhere; literal pools and alignment padding are
              no code. The machine outliner: a short OUTLINED_FUNCTION_<n> that calls nothing is followed at each
              `bl` on the caller's registers (a boot-ROM lookup or a sanitize check it took along); a `bx rN` on a
              register it never writes is its callers' call; its own unresolved calls are its callers' too (a note
              on a caller covers it - no note can name OUTLINED_FUNCTION_<n>). gcc's `ldr.w pc, [rT, rI, lsl #2]` is a
              switch, no call.
  roots       core 0's vector table (the Startup's nvicIsrVectors symbol), core 1's ([_LINKER_INTERN_core1_vectors_*]).
  levels      `.kvasir.isr_levels` (StartUp.hpp, non-allocated): per core {core, n, (index, level) x n} int16, index =
              IRQ number or exception number - 16, level = the priority the Startup list leaves it at (-1: set at run
              time). Without it every handler counts as its own level ("all nest").
  notes       annotation files, merged (SDK, chip, firmware), one directive a line, `#` comments; names mangled,
              demangled or plain (`Kvasir::Foo::bar` for `void Kvasir::Foo<int>::bar(int) const [clone .isra.0]`),
              globs (fnmatch), no spaces (write `*` or `?`):
                frame <fn> <bytes>        the frame of a function the code reading cannot see
                calls <fn> <callee>...    the targets of <fn>'s indirect calls; only a <fn> that has some (an
                                          unresolved one, or one the type hash matched: the note replaces those)
                                          takes the edges, a later `calls` adds to them
                indirect <fn> <bytes>     <fn>'s indirect calls go to code we cannot see: budget them
                rom <fn> [<bytes>]        <fn>'s indirect calls go into the boot ROM: not measured, LOWER BOUND; with
                                          <bytes>, its boot-ROM calls (recognised or indirect) are budgeted at that
                                          (the deepest ROM function it reaches, measured: the chip's notes say how)
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
only its exception frame and its own frame to the interrupted stack. On a stack of its own (Fault::Options::stackBytes:
a `faultStack` object whose end is in its literal pool) its path plus one nested exception frame is checked against
that stack, which must not overlap the main one; below a reserve of the faulting stack, its path against the room
below that SP and the bound against the reserve (the faulting context's frames must survive). A handler that sets SP to
neither fails the check. A bound with any `!` reachable is a LOWER BOUND, and the report says why.

Exit codes: 0, 1 with --check when a bound exceeds its limit (with --exact also when one is a LOWER BOUND), 2 when the
ELF cannot be read. Without --quiet it prints the report; with --quiet only a failed check.
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
RX_JUMP_TABLE = re.compile(r'^pc, \[r\d+, r\d+, lsl #(0x)?2\]$')
# one register pushed / popped with writeback: `str lr, [sp, #-8]!`, `ldr pc, [sp], #8` (hex or decimal)
RX_PUSH_ONE = re.compile(r'\[sp, #-(0x[0-9a-f]+|\d+)\]!')
RX_POP_ONE = re.compile(r'^(\w+), \[sp\], #(0x[0-9a-f]+|\d+)')


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


RX_CLONE = re.compile(r' \[clone [^\]]*\]|\[abi:[^\]]*\]')
RX_QUALIFIER = re.compile(r'\s+(?:const|volatile|noexcept|&&|&)(?=::|\s|$)')
OPERATOR_CHARS = set('<>=!+-*/%^&|~[],')


def plain(name):
    """A demangled name without its return type, template arguments, parameters, trailing qualifiers and gcc's clone
    suffixes, for matching notes: `Kvasir::Foo::bar` for `void Kvasir::Foo<int>::bar(int) const [clone .isra.0]`,
    `Kvasir::Rom::lookup` for `void (*Kvasir::Rom::lookup<'F'>())(int)`, `f::'lambda'::operator()` for a lambda's
    call operator, `ns::{anonymous}::g` for `(anonymous namespace)`."""
    s = RX_CLONE.sub('', name).replace('(anonymous namespace)', '{anonymous}')
    depth, out, ptr, i = 0, [], 0, 0
    while i < len(s):
        c = s[i]
        if depth == 0 and s.startswith('operator', i) and (i == 0 or not (s[i - 1].isalnum() or s[i - 1] == '_')):
            # the operator's own characters are part of the name: operator(), operator<<, operator new
            j = i + len('operator')
            if s.startswith('()', j):
                j += 2
            elif j < len(s) and s[j] == ' ':
                while j < len(s) and s[j] not in '(<':
                    j += 1
            else:
                while j < len(s) and s[j] in OPERATOR_CHARS:
                    j += 1
            out.append(s[i:j].replace(' ', '\0'))
            i = j
            continue
        if depth == 0 and c == '(' and s.startswith('(*', i):
            # R (*name(params))(params): a function returning a function pointer, the name inside the declarator
            ptr += 1
            out.append(' ')
            i += 2
            continue
        if c in '<(':
            depth += 1
        elif c in '>)':
            if depth == 0 and c == ')' and ptr:
                ptr -= 1
            else:
                depth = max(0, depth - 1)
        elif depth == 0:
            out.append(c)
        i += 1
    # a qualifier is no word of the name, at the end or inside it (`f()::'lambda'() const::__invoke`)
    joined = RX_QUALIFIER.sub('', ''.join(out))
    words = joined.split()
    return words[-1].replace('\0', ' ') if words else ''


RX_DATA = re.compile(r'(^|\s)\.(word|short|byte|long|inst|2byte|4byte)\b')
RX_HEXBYTE = re.compile(r'^[0-9a-f]{2}$')


def data_line(op, rest):
    """objdump's line for data inside a function (a literal pool): `00 00 f0 3f  .word 0x3ff00000`."""
    return op.startswith('.') or (RX_HEXBYTE.match(op) is not None and RX_DATA.search(rest) is not None)


CALLEE_SAVED = ('r4', 'r5', 'r6', 'r7', 'r8', 'r9', 'r10', 'r11')
TWO_DESTINATIONS = ('ldrd', 'ldrexd', 'umull', 'smull',
                    'umlal', 'smlal', 'umaal', 'mrrc')
RX_REG = re.compile(r'^(r\d+|lr|ip|sb|sl|fp)$')
REG_ALIAS = {'sb': 'r9', 'sl': 'r10', 'fp': 'r11', 'ip': 'r12'}
NOT_WRITING = ('str', 'cmp', 'cmn', 'tst', 'teq', 'push', 'stm', 'it', 'b', 'cb', 'dmb', 'dsb', 'isb', 'nop', 'vst',
               'pld', 'pli', 'msr', 'svc', 'bkpt', 'cps', 'wfi', 'wfe', 'sev', 'tb')


def written_registers(op, args):
    """The core registers an instruction writes (for the registers a function writes exactly once): the destination,
    the second of a two-register destination, a pop/ldm list, a base register's writeback."""
    out = set()
    regl = [r.strip() for r in register_list(args)]
    if RX_POP.match(op) or op.startswith('ldm'):
        out.update(regl)
        if op.startswith('ldm') and '!' in args.split('{', 1)[0]:
            out.add(args.split(',')[0].strip().rstrip('!'))
    else:
        parts = [p.strip() for p in args.split(',')] if args else []
        if parts and RX_REG.match(parts[0]) and (op.startswith('strex') or not op.startswith(NOT_WRITING)):
            out.add(parts[0])
        if op.startswith(TWO_DESTINATIONS) and len(parts) > 1 and RX_REG.match(parts[1]):
            out.add(parts[1])
        if op.startswith('vmov') and len(parts) > 2 and RX_REG.match(parts[0]) and RX_REG.match(parts[1]):
            out.add(parts[1])
        wb = re.search(r'\[(r\d+|sb|sl|fp|ip)(, [^\]]*)?\](!|, )', args)
        if wb and (op.startswith(('ldr', 'str'))):
            out.add(wb.group(1))
    return {REG_ALIAS.get(r, r) for r in out}


def single_written(lines):
    """The callee-saved registers a function's code writes exactly once (a pop that returns does not count: nothing
    of the function runs after it). Such a register holds that one value wherever it is read: compiled code never reads
    a callee-saved register it did not write but to save it, so the value survives every merge of the linear pass."""
    count = {}
    for _pc, op, rest in lines:
        if data_line(op, rest):
            continue
        args = rest.split('@', 1)[0].strip()
        regs = written_registers(op, args)
        if (RX_POP.match(op) or op.startswith('ldm')) and 'pc' in regs:
            continue
        for r in regs:
            count[r] = count.get(r, 0) + 1
    return {r for r in CALLEE_SAVED if count.get(r) == 1}


def loop_heads(lines, lo, hi):
    """The targets of the backward branches inside the function [lo, hi): loop heads, reached again from below with
    whatever the code after them left in the registers. (A jump table's targets are taken to lie after it, as a
    compiled switch's do.)"""
    heads = set()
    for pc, op, rest in lines:
        if not (RX_B.match(op) or RX_CB.match(op) or RX_BL.match(op)):
            continue
        t = RX_TARGET.search(rest.split('@', 1)[0])
        if t:
            target = int(t.group(1), 16) & ~1
            if lo <= target <= pc < hi:
                heads.add(target)
    return heads


def fresh_state(addr, singles=(), heads=None):
    """The linear pass's state of one function: function, SP offset, registers, literals since a call, SP copies,
    registers written, register states at forward branch targets, "the instruction before does not fall through",
    the registers written exactly once, their values once written, the register states the back edges bring to each
    loop head ({head: dict}, None: not known yet - nothing taken away), and those this pass collects at its back
    edges."""
    return [addr, 0, {}, [], set(), set(), {}, [False], set(singles), {}, heads or {}, {}]


# a register that holds one of a few known values, depending on the way it came: ('set', frozenset of ints)
VALUE_SET_MAX = 8


def constants(v):
    """The known constants a register value stands for: {v} for an int, its members for a value set, None else."""
    if isinstance(v, int):
        return {v}
    if isinstance(v, tuple) and v and v[0] == 'set':
        return set(v[1])
    return None


def join_value(a, b):
    """One register's value where two ways meet: equal values stay, known constants become the set of both (a call
    through it reaches each), anything else is unknown (None)."""
    if a == b:
        return a
    ca, cb = constants(a), constants(b)
    if ca is None or cb is None:
        return None
    both = ca | cb
    return ('set', frozenset(both)) if len(both) <= VALUE_SET_MAX else None


def meet(a, b):
    """The registers two ways into one place bring (a: None for a way not known yet): join_value of each."""
    if a is None:
        return dict(b)
    out = {}
    for k, v in a.items():
        if k in b:
            j = join_value(v, b[k])
            if j is not None:
                out[k] = j
    return out


# passes over one function until its loop heads' states settle; past that nothing is taken as known at them
LOOP_PASSES = 12


# an outlined function this short is followed through at each call: what it leaves in the registers is the caller's
OUTLINED_FOLLOW = 32


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
            elif kind == 'rom' and len(args) in (1, 2):
                if len(args) == 2 and not args[1].isdigit():
                    raise ValueError(
                        f'{where}: rom wants a number of bytes, not {args[1]!r}')
                self.rom.append(
                    (args[0], int(args[1]) if len(args) == 2 else None))
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
                 'literals', 'derived', 'param_calls', 'rom_calls', 'typed_calls', 'via_outlined', 'noted')

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
        # calls into the boot ROM (not measured: a LOWER BOUND, unless a `rom <fn> <bytes>` note budgets them)
        self.rom_calls = 0
        # edges from -fsanitize=function type hashes (a `calls` note replaces them)
        self.typed_calls = set()
        # outlined functions whose unresolved calls were moved here (a note on this function covers them there too)
        self.via_outlined = set()
        # a `calls` note gave this function's targets (a later note adds to them)
        self.noted = False

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
        # the listing grouped by function first (each run of one function's lines, in listing order, data words of
        # its literal pools left out): a function's own pre-pass (the registers it writes once) and the outlined
        # functions a caller's pass follows need the lines before the pass that reads them
        groups = []
        with subprocess.Popen(cmd, stdout=subprocess.PIPE, text=True, errors='replace') as proc:
            cur, lines = None, None
            for line in proc.stdout:
                m = RX_INSN.match(line)
                if not m:
                    continue
                pc = int(m.group(1), 16)
                owner = self.owner(pc)
                if owner is None:
                    cur = None
                    continue
                if owner != cur:
                    cur, lines = owner, []
                    groups.append((owner, lines))
                if not data_line(m.group(2), m.group(3)):
                    lines.append((pc, m.group(2), m.group(3)))
        if proc.returncode:
            raise RuntimeError(
                f'{" ".join(cmd)} exited with {proc.returncode}')
        self._run_groups(groups, typed, init)
        self._resolve_param_calls()
        for f, callee, site in self.pending_returns:
            # a call through what <callee> returned: the boot ROM's function when <callee> (or one it calls) looked it up
            g = self.funcs[callee]
            if 'rom lookup' in g.flags or any(isinstance(c, int) and 'rom lookup' in self.funcs[c].flags for c in g.calls):
                f.rom_calls += 1
            else:
                f.unknown.append(site + ' (what a call returned)')
        self._attribute_outlined()

    def _run_groups(self, groups, typed, init):
        """The linear pass over each function's lines (`groups`: [(function address, [(pc, op, rest)])])."""
        self.outlined = {a: lines for a, lines in groups
                         if self.funcs[a].names[0].startswith('OUTLINED_FUNCTION') and len(lines) <= OUTLINED_FOLLOW}
        for addr, lines in groups:
            f = self.funcs[addr]
            singles = single_written(lines)
            heads = loop_heads(lines, f.addr, f.addr + f.size)
            back = self._settle_loops(
                f, lines, singles, heads, typed, init) if heads else {}
            state = fresh_state(addr, singles, back)
            for pc, op, rest in lines:
                self._step(f, state, pc, op, rest, typed, init)

    def _settle_loops(self, f, lines, singles, heads, typed, init):
        """The register states the back edges bring to each loop head: the linear pass, without its effects (a
        throwaway function, the image's call sites and returns set aside), again with what the back edges brought
        last time, until nothing changes - the states only shrink, so it ends; LOOP_PASSES without settling, and
        nothing is known at the heads. The pass that follows, with its effects, starts from these."""
        back = {t: None for t in heads}
        saved = self.callsites, self.pending_returns
        try:
            for _ in range(LOOP_PASSES):
                self.callsites, self.pending_returns = {}, []
                sink = Function(f.addr, f.size, f.names)
                state = fresh_state(f.addr, singles, back)
                for pc, op, rest in lines:
                    self._step(sink, state, pc, op, rest, typed, init)
                settled = {t: state[11].get(t, {}) for t in heads}
                if settled == back:
                    return back
                back = settled
        finally:
            self.callsites, self.pending_returns = saved
        return {t: {} for t in heads}

    def _step(self, f, state, pc, op, rest, typed, init):
        """One instruction, then the value of a register the function writes once, should this be its write."""
        self._instruction(f, state, pc, op, rest, typed, init)
        if state[8]:
            for r in written_registers(op, rest.split('@', 1)[0].strip()) & state[8]:
                v = state[2].get(r)
                if v is not None and v != 'popped':
                    state[9][r] = v

    def _attribute_outlined(self):
        """An outlined function's own unresolved calls (`ldr r3, [r1, #8]; bx r3`, the dispatch the outliner moved out
        of its callers) are its callers' as much as its own: no note can name OUTLINED_FUNCTION_<n>, a number that
        changes with every build. Each caller takes them, marked with the outlined function, and a note on a caller
        then gives the targets to the outlined function as well (its frame is on the path)."""
        for g in self.funcs.values():
            if not g.unknown or not g.names[0].startswith('OUTLINED_FUNCTION'):
                continue
            callers = {caller for caller, _ in self.callsites.get(g.addr, [])}
            if not callers:
                continue
            for caller in callers:
                c = self.funcs[caller]
                c.unknown += [f'{u} in {g.names[0]}' for u in g.unknown]
                c.via_outlined.add(g.addr)
            g.unknown = []

    def _follow_outlined(self, caller, callee, state, typed, init):
        """`bl OUTLINED_FUNCTION_<n>`: the outliner's code is the caller's, moved out (a `movs r0, #0x16; ldrh r2,
        [r0]` boot-ROM lookup, a sanitize check's literals). A short body that calls nothing is run on a copy of the
        caller's registers and its result is the caller's; one that calls something leaves what a call leaves.
        True when followed."""
        body = self.outlined.get(callee)
        if not body:
            return False
        g = self.funcs[callee]
        sim = Function(g.addr, g.size, [f'<{g.names[0]} for its caller>'])
        sim_state = fresh_state(g.addr)
        sim_state[2].update(state[2])
        sim_state[3].extend(state[3])
        sim_state[4].update(state[4])
        saved = self.callsites, self.pending_returns
        self.callsites, self.pending_returns = {}, []
        try:
            for pc, op, rest in body:
                self._instruction(sim, sim_state, pc, op, rest, typed, init)
        finally:
            self.callsites, self.pending_returns = saved
        if sim.calls or sim.unknown or sim.rom_calls or sim.param_calls or sim.flags & {'sets sp', 'dynamic sp'}:
            return False
        regs = state[2]
        regs.clear()
        regs.update(sim_state[2])
        regs.pop('lr', None)
        state[3][:] = sim_state[3]
        state[5].update(sim_state[5])
        caller.literals += sim.literals
        return True

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
                    members = constants(snap.get(r))
                    if members and all((v & ~1) in self.funcs for v in members):
                        self.funcs[caller].calls |= {v & ~1 for v in members}
                    else:
                        self.funcs[caller].unknown.append(
                            f'{op} {r} @{pc:#x} in {f.names[0]}')

    def _instruction(self, f, state, pc, op, rest, typed, init):
        if op == '<unknown>':
            # an instruction llvm-objdump could not decode for this CPU
            self.undecoded += 1
            return
        if op.startswith('<') or data_line(op, rest):
            # .word / .short: data in the function (literal pool); it neither falls through nor clears anything
            return
        args = rest.split('@', 1)[0].strip()
        regs, lits, spregs, written, targets, dead = state[
            2], state[3], state[4], state[5], state[6], state[7]
        if dead[0] and op in ('nop', 'nop.n', 'nop.w') and pc not in targets:
            # alignment padding after a jump (before a literal pool): nothing runs it
            return
        # the register values one linear pass can know: at a forward branch target the states of both ways meet (only
        # the branch's when the code before it does not fall through: a return, a jump, an error path)
        if pc in targets:
            snap = targets.pop(pc)
            joined = snap if dead[0] else meet(regs, snap)
            regs.clear()
            regs.update(joined)
        elif dead[0]:
            # reached from somewhere this pass has not seen: nothing known
            regs.clear()
        dead[0] = False
        if len(state) > 10 and pc in state[10]:
            # a loop head: what the back edges bring meets what came this way (the literals of a way in are not
            # known on the other)
            back = state[10][pc]
            if back is not None:
                joined = meet(regs, back)
                regs.clear()
                regs.update(joined)
            lits.clear()
        if len(state) > 9:
            # a register the function writes once holds that value wherever it is read (single_written)
            regs.update(state[9])
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
        elif op.startswith('str') and RX_PUSH_ONE.search(args):
            # `str lr, [sp, #-8]!` (libgcc's soft-float compares): llvm-objdump prints a pre- or post-indexed
            # immediate in decimal even with --print-imm-hex, so both spellings
            state[1] += int(RX_PUSH_ONE.search(args).group(1), 0)
        elif op.startswith('ldr') and RX_POP_ONE.match(args):
            mm = RX_POP_ONE.match(args)
            if mm.group(1) == 'pc':
                returns = True                      # `ldr pc, [sp], #8`
            else:
                state[1] -= int(mm.group(2), 0)
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
                if (t & ~1) > pc:
                    targets[t & ~1] = meet(targets.get(t & ~1), regs)
                elif len(state) > 11:
                    state[11][t & ~1] = meet(state[11].get(t & ~1), regs)
                dead[0] = True
                return
            self._edge(f, t)
            callee = self.owner(t & ~1)
            if callee is not None and self.funcs[callee].names[0].startswith('OUTLINED_FUNCTION'):
                self.callsites.setdefault(callee, []).append(
                    (f.addr, {k: v for k, v in regs.items() if constants(v) is not None}))
                if self._follow_outlined(f, callee, state, typed, init):
                    return
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
                # leaves the function: a tail call (into an outlined function: its call site, as for a bl)
                self._edge(f, t)
                callee = self.owner(t & ~1)
                if callee is not None and self.funcs[callee].names[0].startswith('OUTLINED_FUNCTION'):
                    self.callsites.setdefault(callee, []).append(
                        (f.addr, {k: v for k, v in regs.items() if constants(v) is not None}))
            elif t > pc:
                targets[t] = meet(targets.get(t), regs)
            elif len(state) > 11:
                # a back edge: what it brings to the loop head, for the next pass (_run_groups)
                state[11][t] = meet(state[11].get(t), regs)
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
            if RX_JUMP_TABLE.match(args):
                # gcc's switch: `adr rT, table; ldr.w pc, [rT, rI, lsl #2]` - a jump inside the function, no call
                dead[0] = True
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
        # the boot ROM's lookup pointer read: clang `movs r0, #0x16; ldrh r2, [r0]`, gcc `movs r5, #0x0; ldrh r3,
        # [r5, #0x16]`
        mm = re.match(r'^(r\d+), \[(r\d+)(?:, #(0x[0-9a-f]+|\d+))?\]$',
                      args) if op.startswith('ldrh') else None
        if mm and isinstance(regs.get(mm.group(2)), int) and \
                regs[mm.group(2)] + int(mm.group(3) or '0', 0) in self.notes.rom_pointer:
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
        mm = re.match(r'^(r\d+), (r\d+)$', args) if op in ('uxth',
                                                           'uxth.w', 'uxtb', 'uxtb.w') else None
        if mm:
            # a zero-extension (gcc after its ldrh of the boot-ROM pointer): the lookup stays the lookup
            v = regs.get(mm.group(2))
            if v == 'romlookup' and op.startswith('uxth'):
                regs[dest] = v
            elif isinstance(v, int):
                regs[dest] = v & (0xFFFF if op.startswith('uxth') else 0xFF)
            else:
                regs.pop(dest, None)
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
        members = constants(value) if isinstance(value, tuple) else None
        if members and all((v & ~1) in self.funcs or self.in_rom(v) for v in members):
            # one of a few functions, by the way it came (join_value): each is a target
            for v in members:
                if (v & ~1) in self.funcs:
                    f.calls.add(v & ~1)
                else:
                    f.rom_calls += 1
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
        if SANITIZE_SIGNATURE in lits or SANITIZE_SIGNATURE in f.literals:
            # -fsanitize=function: the signature compared against [f-8] may be loaded once (hoisted out of a loop);
            # the type hash is loaded for the call, since the last one - or it is in a register (the union of what
            # the registers hold: every one is a candidate)
            hashes = [x for x in lits if x in typed]
            cands = set(typed[hashes[-1]]) if hashes else set()
            if not cands:
                for v in {v for v in regs.values() if isinstance(v, int) and v in typed}:
                    cands |= typed[v]
            if cands:
                f.calls |= cands
                f.typed_calls |= cands
                f.flags.add(f'typed:{len(cands)}')
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
        self.extra_nodes = {}       # ('budget' | 'rom', fn address) -> bytes
        self.rom = set()
        self.rom_budget = {}        # fn address -> bytes its boot-ROM calls are budgeted at
        for pattern, size in n.frame:
            for f in self.matching(pattern):
                f.record, f.source = size, 'note'
        for pattern, callees in n.calls:
            targets = set()
            for c in callees:
                targets |= {g.addr for g in self.matching(c)}
            for f in self.matching(pattern):
                # the targets of what is indirect here - a function whose calls are all direct (a glob over a class
                # matches its every member) takes no edges, they would only lengthen paths and close false cycles
                if not (f.unknown or f.typed_calls or f.noted):
                    continue
                # the note names the targets: it replaces what the type hash matched (`void()` is every ISR)
                f.calls -= f.typed_calls - targets
                f.calls |= targets
                for g in f.via_outlined:
                    self.funcs[g].calls |= targets
                f.unknown = []
                f.noted = True
        for pattern, size in n.indirect:
            for f in self.matching(pattern):
                if f.unknown:
                    self.extra_nodes[('budget', f.addr)] = size
                    f.calls.add(('budget', f.addr))
                    for g in f.via_outlined:
                        self.funcs[g].calls.add(('budget', f.addr))
                    f.unknown = []
        budgeted = {f.addr for pattern,
                    size in n.rom if size is not None for f in self.matching(pattern)}
        for pattern, size in n.rom:
            for f in self.matching(pattern):
                foreign = {c for c in f.calls if isinstance(
                    c, tuple) and c[0] == '?'}
                if not (f.unknown or foreign or f.rom_calls):
                    continue
                if size is None:
                    # its indirect calls go into the boot ROM: not measured (a LOWER BOUND) - unless another `rom`
                    # line budgets the same function (a glob over a class, a budget for one member)
                    if (f.unknown or foreign) and f.addr not in budgeted:
                        self.rom.add(f.addr)
                    continue
                # budgeted: the deepest boot-ROM function it calls, measured elsewhere (the chip's notes say where)
                node = ('rom', f.addr)
                self.extra_nodes[node] = max(
                    self.extra_nodes.get(node, 0), size)
                self.rom_budget[f.addr] = self.extra_nodes[node]
                f.calls -= foreign
                f.calls.add(node)
                for g in f.via_outlined:
                    self.funcs[g].calls.add(node)
                f.unknown = []
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
            if node[0] == 'rom':
                return f'<boot ROM calls of {short(self.funcs[node[1]].name)}>'
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
        if f.rom_calls and node not in self.rom_budget:
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


def own_stack(image, handler, lo, hi):
    """The stack of its own a Fault::HandlerFor<{.stackBytes = N}> runs on: the `faultStack` object (FaultHandler.hpp)
    whose end is in the handler's literal pool (`ldr r0, =top; mov sp, r0`). None without one."""
    lits = set(image.funcs[handler].literals)
    for s in image.elf.symbols:
        if s['type'] == STT_OBJECT and 'faultStack' in s['name'] and s['size'] and s['value'] + s['size'] in lits:
            bottom, top = s['value'], s['value'] + s['size']
            return {'symbol': s['name'], 'bottom': bottom, 'top': top, 'size': s['size'],
                    'overlaps': None not in (lo, hi) and bottom < hi and lo < top}
    return None


def fault_failures(core, fault, frame, bound, fault_class, lo, hi):
    """The fault handler's rules for one core: `fault` is the report's fault entry (None: no handler sets its SP),
    `frame` the exception frame, `bound` the core's bound and `fault_class` the part of it the fault class takes."""
    if not fault:
        return []
    out = []
    own, room = fault.get('own_stack'), fault['room']
    if own:
        # a stack of its own: the faulting context's frames are untouched however deep it was; what is checked is
        # the handler's path in it, with one more exception frame (a fault in a configurable-priority handler
        # escalates to HardFault on the same SP)
        if fault['depth'] + frame > own['size']:
            out.append(f'core {core}: fault path {fault["depth"]} B + a nested exception frame {frame} B > its own '
                       f'stack {own["size"]} B (Fault::Options::stackBytes)')
        if own['overlaps']:
            out.append(f'core {core}: the fault handler\'s own stack [{own["bottom"]:#x}, {own["top"]:#x}) overlaps '
                       f'the main stack [{lo:#x}, {hi:#x})')
    elif room:
        if fault['depth'] > room['room']:
            out.append(f'core {core}: fault path {fault["depth"]} B > the {room["room"]} B below the fault handler\'s '
                       f'SP')
        live = bound - fault_class
        if live > room['effective_reserve']:
            out.append(f'core {core}: bound without the fault class {live} B > the fault reserve '
                       f'{room["effective_reserve"]} B (a fault would overwrite the live frames)')
    else:
        out.append(f'core {core}: {short(fault["handler"], 60)} sets SP to a stack that is neither this core\'s (both '
                   f'ends in its literal pool) nor a Fault::Options::stackBytes one: its path is unchecked')
    return out


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
                own = own_stack(image, h, lo, hi)
                room = None if own else fault_room(image, h, hi, lo,
                                                   protector if core == 0 else 0)
                fault = {'handler': f.name, 'vectors': vectors, 'depth': image.depth[h], 'on_stack': frame + f.frame,
                         'room': room, 'own_stack': own, 'lower': lower(image.reasons[h]),
                         'reasons': sorted(image.reasons[h]), 'path': image.chain(h)}
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
        result['failures'] += fault_failures(core, fault, frame, bound,
                                             sum(g['depth'] for k, g in groups.items() if k == ('fault',)), lo, hi)
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
            room, own = fl['room'], fl.get('own_stack')
            lines.append(f'        fault {short(fl["handler"], 50)} (own SP) {fl["depth"]} B'
                         + (' LOWER BOUND' if fl['lower'] else '')
                         + (f'; its own stack {own["size"]} B at {own["bottom"]:#x}' if own else
                            f'; room below its SP {room["room"]} B, reserve {room["effective_reserve"]} B'
                            if room else '; its stack not identified')
                         + f'; {fl["on_stack"]} B on the interrupted stack')
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
    ap.add_argument('--exact', action='store_true',
                    help='also exit 1 when a bound is a LOWER BOUND (implies --check)')
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


def exact_failures(report):
    """--exact: every core whose bound (or fault path) is a LOWER BOUND fails, with the first reasons."""
    for c in report['cores']:
        bang = [w for w in c['reasons'] if w.startswith('!')]
        if c['lower']:
            c['failures'].append(f'core {c["core"]}: LOWER BOUND, {len(bang)} unresolved: ' + '; '.join(bang[:3])
                                 + (' ...' if len(bang) > 3 else ''))
        fl = c['fault']
        if fl and fl['lower']:
            fb = [w for w in fl['reasons'] if w.startswith('!')]
            c['failures'].append(
                f'core {c["core"]}: the fault path is a LOWER BOUND: ' + '; '.join(fb[:3]))


def run(args, elf=None):
    notes = Notes()
    check = args.check or getattr(args, 'exact', False)
    try:
        tool = args.objdump or Image._find_objdump()
    except FileNotFoundError as e:
        # no llvm-objdump on this machine: no report, one line in its place - not a link failure unless asked to check
        if args.txt:
            args.txt.write_text(f'{args.elf.name}: no stack bound: {e}\n')
        if check:
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
    if getattr(args, 'exact', False):
        exact_failures(report)
    text = text_report(report, args.path)
    if args.txt:
        args.txt.write_text(text)
    if args.json:
        args.json.write_text(json.dumps(report, indent=1) + '\n')
    failures = [f for c in report['cores'] for f in c['failures']]
    if not args.quiet:
        sys.stdout.write(text)
    if check and failures:
        if args.quiet:
            sys.stderr.write(text)
        print(f'stack_bound: {args.elf.name}: STACK_BOUND_CHECK failed: ' +
              '; '.join(failures), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
