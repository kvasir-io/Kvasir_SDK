#!/usr/bin/env python3
"""cmake/tools/stack_bound.py (the post-link static stack bound): the graph rules on hand-made functions and recorded
llvm-objdump lines (no toolchain), the record parsers, and crafted images linked here - clang + ld.lld with
--mllvm=-stack-size-section, gcc's link-time -fcallgraph-info=su - against the frames the toolchain itself reports
(llvm-readobj --stack-sizes). A missing toolchain is a skip (tests/tooling/armtc.py)."""

import json
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
SDK = HERE.parents[1]
sys.path.insert(0, str(SDK / 'cmake' / 'tools'))
sys.path.insert(0, str(SDK / 'tests' / 'tooling'))
import armtc  # noqa: E402
import stack_bound as sb  # noqa: E402


def bare_image(funcs, notes=None, words=None):
    """An Image without an ELF: functions {address: (name, frame)}, the graph filled in by the test."""
    im = sb.Image.__new__(sb.Image)
    im.notes = notes or sb.Notes()
    im.funcs = {}
    for addr, (name, frame) in funcs.items():
        f = sb.Function(addr, 0x40, [name])
        f.keys = [name]
        f.record, f.source = frame, 'su'
        im.funcs[addr] = f
    im.starts = sorted(im.funcs)
    im.byname = {f.names[0]: a for a, f in im.funcs.items()}
    im.extra_nodes, im.rom, im.once, im.recurse, im.fault_class = {}, set(), set(), {
    }, set()
    im.code_ranges = [(0x10000000, 0x10010000)]
    im.callsites, im.pending_returns, im.noreturn = {}, [], set()
    im.words = words or {}
    im.word = lambda a: im.words.get(a)
    im.fp_seen = False
    return im


def run_lines(im, f, lines, typed=None, init=(None, None)):
    """Feeds recorded `llvm-objdump -d --no-show-raw-insn --print-imm-hex` lines of one function to the parser."""
    state = [f.addr, 0, {}, [], set(), set(), {}, [False]]
    for line in lines:
        m = sb.RX_INSN.match(line)
        im._instruction(f, state, int(m.group(1), 16),
                        m.group(2), m.group(3), typed or {}, init)
    return state


class Graph(unittest.TestCase):
    def test_longest_path_and_recursion(self):
        im = bare_image({0x100: ('a', 8), 0x200: ('b', 16),
                        0x300: ('c', 32), 0x400: ('d', 4)})
        im.funcs[0x100].calls = {0x200, 0x400}
        im.funcs[0x200].calls = {0x300}
        im.funcs[0x400].calls = {0x400}               # d calls itself
        im.solve()
        self.assertEqual(im.depth[0x100], 8 + 16 + 32)
        self.assertEqual([n for _, n in im.chain(0x100)], ['a', 'b', 'c'])
        self.assertTrue(any(r.startswith('!recursion: d')
                        for r in im.reasons[0x100]))
        im.recurse = {0x400: 5}
        im.solve()
        self.assertEqual(im.depth[0x400], 5 * 4)
        self.assertFalse(sb.lower(im.reasons[0x400]))

    def test_a_three_cycle_through_an_error_path_counts_once(self):
        # log -> fail (once) -> log2 -> log: without `once` a recursion, with it the error path once
        im = bare_image({0x100: ('main', 8), 0x200: ('log', 40),
                        0x300: ('fail', 16), 0x400: ('log2', 24)})
        im.funcs[0x100].calls = {0x200}
        im.funcs[0x200].calls = {0x300}
        im.funcs[0x300].calls = {0x400}
        im.funcs[0x400].calls = {0x200}
        im.solve()
        self.assertTrue(any(r.startswith('!recursion')
                        for r in im.reasons[0x100]))
        im.once = {0x300}
        im.solve()
        # main, log, then the error path once: fail, log2, log (its error entry into fail stops)
        self.assertEqual(im.depth[0x100], 8 + 40 + 16 + 24 + 40)
        self.assertFalse(sb.lower(im.reasons[0x100]))

    def test_unresolved_calls_make_a_lower_bound(self):
        im = bare_image({0x100: ('a', 8), 0x200: ('b', 16)})
        im.funcs[0x100].calls = {0x200, ('?', 0x4252)}
        im.funcs[0x200].unknown = ['blx r3 @0x204']
        im.solve()
        reasons = sorted(im.reasons[0x100])
        self.assertTrue(sb.lower(reasons))
        self.assertTrue(
            any('b: indirect call blx r3' in r for r in reasons), reasons)
        self.assertTrue(
            any('call into 0x00004252' in r for r in reasons), reasons)


class Parsers(unittest.TestCase):
    def test_stack_sizes_with_a_two_byte_uleb(self):
        blob = struct.pack(
            '<I', 0x10000101) + bytes([0x90, 0x03]) + struct.pack('<I', 0x10000200) + bytes([8])
        self.assertEqual(sb.parse_stack_sizes(blob), {
                         0x10000100: 400, 0x10000200: 8})

    def test_ci(self):
        text = ('graph: { title: "/tmp/x.ltrans0.o"\n'
                'node: { title: "/tmp/x.ltrans0.o:f.constprop.0" label: "f.constprop\\nt.c:1:47\\n104 bytes (static)" }\n'
                'node: { title: "main" label: "main\\nt.c:2:5\\n8 bytes (dynamic,bounded)" }\n'
                'node: { title: "__indirect_call" label: "Indirect call" }\n'
                'edge: { sourcename: "main" targetname: "/tmp/x.ltrans0.o:f.constprop.0" }\n}\n')
        self.assertEqual(sb.parse_ci(text), {'f.constprop.0': (
            104, 'static'), 'main': (8, 'dynamic,bounded')})

    def test_register_lists(self):
        self.assertEqual(sb.register_list('{r4, r5, r7, lr}'), [
                         'r4', 'r5', 'r7', 'lr'])
        self.assertEqual(sb.register_list('{d8-d10}'), ['d8', 'd9', 'd10'])

    def test_notes(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / 'n.txt'
            p.write_text('# c\nonce __ubsan_handle_*\nrecurse f 3\ncalls g h i\nrom_pointer 0x16\n'
                         'rom_region 0x0 0x8000\n')
            n = sb.Notes()
            n.read(p)
            self.assertEqual((n.once, n.recurse, n.calls, n.rom_pointer, n.rom_region),
                             (['__ubsan_handle_*'], [('f', 3)], [('g', ['h', 'i'])], [0x16], [(0, 0x8000)]))
            p.write_text('frame f lots\n')
            with self.assertRaises(ValueError):
                sb.Notes().read(p)


class Instructions(unittest.TestCase):
    """The resolutions of stack_bound.py's docstring, on recorded objdump lines."""

    def image(self, extra=None, notes=None, words=None):
        funcs = {0x10000000: ('f', 0), 0x10001000: (
            'target', 8), 0x10002000: ('other', 16)}
        funcs.update(extra or {})
        return bare_image(funcs, notes, words)

    def test_the_frame_from_the_code(self):
        im = self.image()
        f = im.funcs[0x10000000]
        run_lines(im, f, ['10000000:  push {r4, r5, r7, lr}', '10000002:  sub sp, #0x40',
                          '10000004:  add sp, #0x40', '10000006:  pop {r4, r5, r7, pc}'])
        self.assertEqual(f.code, 16 + 0x40)

    def test_gcc_v6m_constant_frames_and_pop_bx(self):
        im = self.image(words={0x10000100: 0xFFFFFE00})
        f = im.funcs[0x10000000]
        run_lines(im, f, ['10000000:  push {r4, lr}', '10000002:  ldr r4, [pc, #0xfc]  @ 0x10000100',
                          '10000004:  add sp, r4', '10000006:  pop {r3}', '10000008:  bx r3'])
        self.assertEqual(f.code, 8 + 0x200)
        self.assertEqual((f.unknown, f.flags), ([], set()))

    def test_literal_pool_call(self):
        im = self.image(words={0x10000100: 0x10001001})
        f = im.funcs[0x10000000]
        run_lines(
            im, f, ['10000000:  ldr r5, [pc, #0xfc]  @ 0x10000100', '10000002:  blx r5'])
        self.assertEqual(f.calls, {0x10001000})

    def test_a_value_that_is_no_function_is_unresolved(self):
        # a RAM address left in the register
        im = self.image(words={0x10000100: 0x20001724})
        f = im.funcs[0x10000000]
        run_lines(
            im, f, ['10000000:  ldr r1, [pc, #0xfc]  @ 0x10000100', '10000002:  blx r1'])
        self.assertEqual(f.calls, set())
        self.assertEqual(f.unknown, ['blx r1 @0x10000002'])

    def test_calls_clobber_the_argument_registers(self):
        im = self.image(words={0x10000100: 0x10001001})
        f = im.funcs[0x10000000]
        run_lines(im, f, ['10000000:  ldr r2, [pc, #0xfc]  @ 0x10000100', '10000002:  bl 0x10002000 <other>',
                          '10000006:  blx r2'])
        self.assertEqual(f.calls, {0x10002000})
        self.assertEqual(len(f.unknown), 1)

    def test_tail_calls_and_a_far_jump_inside(self):
        im = self.image()
        f = im.funcs[0x10000000]
        run_lines(im, f, ['10000000:  bl 0x10000020 <f+0x20>',
                  '10000020:  b.w 0x10001000 <target>'])
        self.assertEqual(f.calls, {0x10001000})

    def test_sanitize_type_hash(self):
        im = self.image(extra={0x10003000: ('lambda1', 8), 0x10004000: ('lambda2', 8)},
                        words={0x10000100: sb.SANITIZE_SIGNATURE, 0x10000104: 0x1234ABCD})
        f = im.funcs[0x10000000]
        typed = {0x1234ABCD: {0x10003000, 0x10004000}}
        run_lines(im, f, ['10000000:  ldr r1, [r0, #-8]', '10000004:  ldr r2, [pc, #0xf8]  @ 0x10000100',
                          '10000006:  cmp r1, r2', '10000008:  ldr r0, [r0, #-4]',
                          '1000000c:  ldr r1, [pc, #0xf4]  @ 0x10000104', '1000000e:  blx r11'], typed=typed)
        self.assertEqual(f.calls, {0x10003000, 0x10004000})

    def test_the_static_constructor_loop(self):
        im = self.image(words={0x10000100: 0x10007000})
        im.init_array = [0x10002000]
        f = im.funcs[0x10000000]
        run_lines(im, f, ['10000000:  ldr r5, [pc, #0xfc]  @ 0x10000100', '10000002:  ldm r5!, {r3}',
                          '10000004:  blx r3'], init=(0x10007000, 0x10007004))
        self.assertEqual(f.calls, {0x10002000})

    def test_sp_moves(self):
        im = self.image()
        f = im.funcs[0x10000000]
        run_lines(im, f, ['10000000:  add r7, sp, #0x8',
                  '10000002:  mov sp, r7'])
        self.assertNotIn('sets sp', f.flags)
        run_lines(im, f, ['10000000:  mov sp, r0'])
        self.assertIn('sets sp', f.flags)
        g = im.funcs[0x10001000]
        run_lines(im, g, ['10001000:  sub sp, r3'])
        self.assertIn('dynamic sp', g.flags)
        h = im.funcs[0x10002000]
        run_lines(im, h, ['10002000:  msr msplim, r0'])
        self.assertNotIn('sets sp', h.flags)

    def test_the_boot_rom_lookup(self):
        notes = sb.Notes()
        notes.rom_pointer, notes.rom_region = [0x18], [(0, 0x4000)]
        im = self.image(notes=notes)
        f = im.funcs[0x10000000]
        run_lines(im, f, ['10000000:  movs r0, #0x18', '10000002:  ldrh r2, [r0]', '10000004:  blx r2',
                          '10000006:  blx r0'])
        self.assertEqual((f.rom_calls, f.unknown), (2, []))

    def test_an_outlined_tail_call_through_the_callers_register(self):
        im = self.image(extra={0x10005000: ('OUTLINED_FUNCTION_3', 0)}, words={
                        0x10000100: 0x10001001})
        caller, outlined = im.funcs[0x10000000], im.funcs[0x10005000]
        run_lines(im, caller, ['10000000:  ldr r7, [pc, #0xfc]  @ 0x10000100',
                               '10000002:  bl 0x10005000 <OUTLINED_FUNCTION_3>'])
        run_lines(im, outlined, ['10005000:  mov r0, r5', '10005002:  bx r7'])
        im._resolve_param_calls()
        self.assertIn(0x10001000, caller.calls)
        self.assertEqual(outlined.unknown, [])

    def test_a_branch_target_takes_the_branch_state(self):
        # gcc: bne over an assert path; the register live at the target is the one before the branch
        notes = sb.Notes()
        notes.rom_pointer, notes.rom_region = [0x18], [(0, 0x4000)]
        im = self.image(extra={0x10006000: ('__assert_func', 8)}, notes=notes)
        im.noreturn = {0x10006000}
        f = im.funcs[0x10000000]
        run_lines(im, f, ['10000000:  movs r3, #0x18', '10000002:  ldrh r3, [r3]', '10000004:  blx r3',
                          '10000006:  cmp r0, #0x0', '10000008:  bne 0x10000010 <f+0x10>',
                          '1000000a:  bl 0x10006000 <__assert_func>', '10000010:  blx r0'])
        self.assertEqual((f.rom_calls, f.unknown), (2, []))

    def test_thunk_names(self):
        for name, target in (('__Thumbv7ABSLongThunk_memcpy', 'memcpy'), ('__ThumbV6MABSLongThunk_memcpy', 'memcpy'),
                             ('__memcpy_veneer', 'memcpy')):
            m = sb.RX_THUNK.match(name)
            self.assertEqual(m.group(1) or m.group(2), target)


class Crafted(unittest.TestCase):
    """image.c linked here: every number is what the source asks for, checked against the toolchain's own report."""

    def link_clang(self, d, target='thumbv6m-none-eabi', defines=(), extra=()):
        armtc.need(self, 'clang', 'ld.lld')
        armtc.need(self, 'llvm-objdump')
        obj, elf = Path(d) / 'image.o', Path(d) / 'image.elf'
        subprocess.run(['clang', f'--target={target}', '-Oz', '-flto', '-ffreestanding', *extra,
                        *[f'-D{x}' for x in defines], '-c', str(HERE / 'image.c'), '-o', str(obj)], check=True)
        subprocess.run(['ld.lld', '--mllvm=-stack-size-section', '--gc-sections', '-e', 'ResetISR', '-T',
                        str(HERE / 'image.ld'), str(obj), '-o', str(elf)], check=True)
        return elf

    @staticmethod
    def analyse(elf, notes=''):
        with tempfile.NamedTemporaryFile('w', suffix='.txt', delete=False) as n:
            n.write(notes)
        sn = sb.Notes()
        sn.read(n.name)
        Path(n.name).unlink()
        im = sb.Image(elf, notes=sn)
        return sb.analyse(im), im

    @staticmethod
    def readobj_frames(elf):
        """{function: bytes} from llvm-readobj --stack-sizes, the oracle of the .stack_sizes parser."""
        out = subprocess.run(['llvm-readobj', '--stack-sizes', str(elf)], check=True, capture_output=True,
                             text=True).stdout
        names, frames = None, {}
        for line in out.splitlines():
            line = line.strip()
            if line.startswith('Functions:'):
                names = line.split('[', 1)[1].rstrip(']').split(', ')
            elif line.startswith('Size:') and names:
                for n in names:
                    frames[n] = int(line.split()[1], 0)
        return frames

    def test_deep_chain_handlers_recursion_indirect(self):
        with tempfile.TemporaryDirectory() as d:
            elf = self.link_clang(d)
            frames = self.readobj_frames(elf)
            report, im = self.analyse(elf)
            self.assertEqual(
                {f.name: f.record for f in im.funcs.values()}, frames)
            c = report['cores'][0]
            chain = frames['ResetISR'] + frames['deep1'] + \
                frames['deep2'] + frames['deep3']
            self.assertEqual(c['entry']['depth'], chain)
            self.assertEqual([n for _, n in c['entry']['path']], [
                             'ResetISR', 'deep1', 'deep2', 'deep3'])
            big = frames['bigIsr'] + sb.BASIC_FRAME
            small = frames['smallIsr'] + sb.BASIC_FRAME
            self.assertEqual(sorted(g['depth']
                             for g in c['levels']), sorted([big, small]))
            # no level table: every handler nests
            self.assertEqual(c['bound'], chain + big + small)
            self.assertEqual(c['one_level'], chain + big)
            self.assertTrue(c['lower'])
            self.assertTrue(any(r.startswith('!recursion: rec')
                            for r in c['reasons']), c['reasons'])
            self.assertTrue(any(r.startswith('!ResetISR: indirect call')
                            for r in c['reasons']), c['reasons'])
            # the plan's numbers with clang 23 at -Oz (a newer clang may lay the frames out otherwise)
            self.assertEqual(
                (c['entry']['depth'], big, c['bound']), (304, 436, 780))

            # notes close both: the pointer's one target, the recursion bounded at 4
            report, im = self.analyse(
                elf, 'calls ResetISR smallIsr\nrecurse rec 4\n')
            c = report['cores'][0]
            self.assertFalse(c['lower'], c['reasons'])
            self.assertEqual(im.depth[im.byname['rec']], 4 * frames['rec'])

    def test_the_level_table_and_the_image_bytes(self):
        armtc.need(self, 'llvm-objcopy', 'llvm-readelf')
        with tempfile.TemporaryDirectory() as d:
            with_table = self.link_clang(d, defines=['WITH_LEVELS'])
            renamed = Path(d) / 'levels.elf'
            with_table.rename(renamed)
            without = self.link_clang(d)
            frames = self.readobj_frames(renamed)
            report, _ = self.analyse(renamed)
            c = report['cores'][0]
            self.assertTrue(report['level_table'])
            # all three IRQs at level 1: one level, the deepest of its handlers
            self.assertEqual(c['bound'], c['entry']['depth'] +
                             frames['bigIsr'] + sb.BASIC_FRAME)
            self.assertEqual([g['level'] for g in c['levels']], ['level 1'])
            flags = subprocess.run(['llvm-readelf', '-SW', str(renamed)], check=True, capture_output=True,
                                   text=True).stdout
            line = next(x for x in flags.splitlines()
                        if '.kvasir.isr_levels' in x)
            self.assertNotIn(' A', line.split('PROGBITS', 1)[1][-14:], line)
            bins = []
            for elf in (renamed, without):
                out = Path(d) / (elf.stem + '.bin')
                subprocess.run(['llvm-objcopy', '-O', 'binary',
                               str(elf), str(out)], check=True)
                bins.append(out.read_bytes())
            self.assertEqual(bins[0], bins[1])

    def test_the_fp_frame(self):
        with tempfile.TemporaryDirectory() as d:
            elf = self.link_clang(d, target='thumbv8m.main-none-eabihf', defines=['WITH_FP'],
                                  extra=['-mcpu=cortex-m33', '-mfloat-abi=hard', '-mfpu=fpv5-sp-d16'])
            report, _ = self.analyse(elf)
            self.assertTrue(report['fp_instructions'])
            self.assertEqual(report['exception_frame'], 108)

    def test_gcc_link_time_call_graph(self):
        armtc.need(self, 'arm-none-eabi-gcc')
        armtc.need(self, 'llvm-objdump')
        with tempfile.TemporaryDirectory() as d:
            obj, elf = Path(d) / 'image.o', Path(d) / 'image.elf'
            cpu = ['-mcpu=cortex-m0plus', '-mthumb',
                   '-Os', '-flto', '-ffreestanding']
            subprocess.run(['arm-none-eabi-gcc', *cpu, '-c',
                           str(HERE / 'image.c'), '-o', str(obj)], check=True)
            subprocess.run(['arm-none-eabi-gcc', *cpu, '-nostdlib', '-fstack-usage', '-fcallgraph-info=su',
                            f'-Wl,-T,{HERE / "image.ld"}', '-Wl,--gc-sections', '-Wl,-e,ResetISR', str(
                                obj), '-o',
                            str(elf)], check=True)
            self.assertTrue(list(Path(d).glob('image.elf.ltrans*.ltrans.ci')))
            report, im = self.analyse(elf)
            self.assertEqual(report['records'], '.ci')

            def fn(name):   # gcc's clones: deep1.constprop.0
                return next(f for f in im.funcs.values() if f.names[0].split('.')[0] == name)
            for name in ('deep1', 'deep2', 'deep3', 'bigIsr'):
                f = fn(name)
                self.assertEqual(f.source, 'ci', name)
                # the frame used is max(record, code): gcc leaves out an argument area the code reading sees
                self.assertGreaterEqual(f.frame, f.record)
            self.assertGreaterEqual(fn('deep3').frame, 200)
            c = report['cores'][0]
            self.assertEqual([n.split('.')[0] for _, n in c['entry']['path']], [
                             'ResetISR', 'deep1', 'deep2', 'deep3'])

    def test_check_fails_over_the_limit(self):
        with tempfile.TemporaryDirectory() as d:
            elf = self.link_clang(d)
            args = ['--no-default-notes', '--json',
                    str(Path(d) / 'r.json'), '--quiet', '--check']
            # no MIN_STACK_SIZE symbol: nothing to check against
            self.assertEqual(sb.main([str(elf), *args]), 0)
            self.assertEqual(json.loads(
                (Path(d) / 'r.json').read_text())['cores'][0]['failures'], [])
            # with one below the bound
            subprocess.run(['ld.lld', '--mllvm=-stack-size-section', '--gc-sections', '-e', 'ResetISR',
                            '--defsym=cmake_min_stack_size=256', '-T', str(
                                HERE / 'image.ld'),
                            str(Path(d) / 'image.o'), '-o', str(elf)], check=True)
            self.assertEqual(sb.main([str(elf), *args]), 1)
            failures = json.loads(
                (Path(d) / 'r.json').read_text())['cores'][0]['failures']
            self.assertEqual(len(failures), 1)
            self.assertIn('MIN_STACK_SIZE 256 B', failures[0])


class NoObjdump(unittest.TestCase):
    def test_a_missing_objdump_is_a_note_not_a_failure(self):
        with tempfile.TemporaryDirectory() as d:
            txt = Path(d) / 'r.txt'
            elf = Path(d) / 'x.elf'
            elf.write_bytes(b'')
            path = shutil.which
            try:
                sb.shutil.which = lambda _name: None
                self.assertEqual(
                    sb.main([str(elf), '--txt', str(txt), '--quiet']), 0)
                self.assertIn('no stack bound', txt.read_text())
                self.assertEqual(
                    sb.main([str(elf), '--txt', str(txt), '--quiet', '--check']), 1)
            finally:
                sb.shutil.which = path


if __name__ == '__main__':
    unittest.main()
