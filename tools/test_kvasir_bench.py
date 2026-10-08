#!/usr/bin/env python3
"""kvasir_bench.py's parts that need no board: profile ranking, chip commands."""
import contextlib
import importlib.util
import io
import shutil
import sys
import tempfile
import unittest
import unittest.mock
from pathlib import Path

sys.dont_write_bytecode = True  # no __pycache__ next to the imported tools

HERE = Path(__file__).resolve().parent

spec = importlib.util.spec_from_file_location(
    "kvasir_bench", HERE / "kvasir_bench.py")
assert spec is not None and spec.loader is not None
kb = importlib.util.module_from_spec(spec)
spec.loader.exec_module(kb)


class Profile(unittest.TestCase):
    def test_templates_and_parameters_go(self):
        self.assertEqual(kb.strip_templates(
            "a::B<c<int>, 3>::f(d<e>, int) const"), "a::B::f() const")

    def test_operators_namespaces_and_lambdas_stay(self):
        for name, short in [("(anonymous namespace)::f(int)", "(anonymous namespace)::f()"),
                            ("a::operator<<<int>(x)", "a::operator<<()"),
                            ("a::operator<(A<int>, int)", "a::operator<()"),
                            ("B<c>::operator->() const", "B::operator->() const"),
                            ("f()::{lambda(int)#1}::operator()(int) const",
                             "f()::{lambda(int)#1}::operator()() const"),
                            ("A<(3 > 2)>::h(int)", "A::h()")]:
            self.assertEqual(kb.strip_templates(name), short)

    @unittest.skipUnless(shutil.which("llvm-cxxfilt") or shutil.which("c++filt"), "no demangler")
    def test_mangled_names_behind_prefixes(self):
        short = kb.short_function_names(
            {"", "main", "_ZN6Kvasir4Test3runILi3EEEvi", "__Thumbv7ABSLongThunk__ZN6Kvasir4Test3runEv",
             ".Lswitch.table._ZNK6Kvasir3Foo3barEv"})
        self.assertEqual(short[""], "??")
        self.assertEqual(short["main"], "main")
        self.assertEqual(
            short["_ZN6Kvasir4Test3runILi3EEEvi"], "void Kvasir::Test::run()")
        self.assertEqual(short["__Thumbv7ABSLongThunk__ZN6Kvasir4Test3runEv"],
                         "__Thumbv7ABSLongThunk_Kvasir::Test::run()")
        self.assertEqual(short[".Lswitch.table._ZNK6Kvasir3Foo3barEv"],
                         ".Lswitch.table.Kvasir::Foo::bar() const")

    def test_same_file_name_in_two_folders(self):
        frames = {0x100: [{"FunctionName": "f()", "FileName": "/s/I2C/Device.hpp", "Line": 3}],
                  0x200: [{"FunctionName": "g()", "FileName": "/s/SPI/Device.hpp", "Line": 3}]}
        ranked = kb.rank_samples({0x100: 1, 0x200: 1}, frames, top=5)
        self.assertEqual(sorted(name for _, name in ranked["line"]),
                         ["I2C/Device.hpp:3", "SPI/Device.hpp:3"])

    def test_ranking(self):
        frames = {
            0x100: [{"FunctionName": "leaf<int>()", "FileName": "/x/Engine.hpp", "Line": 10},
                    {"FunctionName": "outer()", "FileName": "/x/Device.hpp", "Line": 5}],
            0x200: [{"FunctionName": "outer()", "FileName": "/x/Device.hpp", "Line": 7}],
        }
        ranked = kb.rank_samples({0x100: 3, 0x200: 1, 0x300: 4}, frames, top=5)
        self.assertEqual(ranked["self"], [(50.0, "??"),
                         (37.5, "leaf()"), (12.5, "outer()")])
        # once per function on the inline stack
        self.assertIn((50.0, "outer()"), ranked["inclusive"])
        self.assertIn((37.5, "Engine.hpp:10"), ranked["line"])

    NM = ("10000100 00000004 T std::optional<unsigned short>::has_value() const\n"
          "10000100 00000004 t FPBits<float>::is_neg() const\n"
          "10000100 00000004 t std::optional<bool>::has_value() const\n"
          "10000200 00000010 T outer()\n"
          "20000000 00000004 B a_variable\n")

    def table(self):
        original = kb.nm
        kb.nm = lambda tree, demangle=True: self.NM
        try:
            return kb.code_symbols(None)
        finally:
            kb.nm = original

    def test_folded_bodies_are_one_symbol_with_every_name(self):
        table = self.table()
        self.assertEqual([start for start, _, _ in table],
                         [0x10000100, 0x10000200])
        self.assertEqual(len(table[0][2]), 3)

    def test_an_address_belongs_to_the_body_it_lies_in(self):
        table = self.table()
        self.assertEqual(kb.symbol_at(table, 0x10000102)[0], 0x10000100)
        self.assertEqual(kb.symbol_at(table, 0x1000020e)[0], 0x10000200)
        self.assertIsNone(kb.symbol_at(table, 0x10000104))   # between the two
        self.assertIsNone(kb.symbol_at(table, 0x20000000))   # data, not code

    def test_ranking_by_symbol_names_the_folded_body(self):
        ranked = kb.rank_by_symbol(
            {0x10000100: 3, 0x10000102: 3, 0x10000204: 2}, self.table(), top=5)
        self.assertEqual(ranked[1], (25.0, "outer()"))
        self.assertAlmostEqual(ranked[0][0], 75.0)
        self.assertIn("[3 folded]", ranked[0][1])
        self.assertIn("is_neg", ranked[0][1])

    def test_a_zero_size_label_is_not_a_folded_function(self):
        nm_out = ("10000130 00000000 T _LINKER_INTERN_core1_vectors_end_\n"
                  "10000130 00012018 t main_body\n"
                  "10000400 00000000 T asm_stub\n")
        original = kb.nm
        kb.nm = lambda tree, demangle=True: nm_out
        try:
            table = kb.code_symbols(None)
        finally:
            kb.nm = original
        self.assertEqual(
            table, [(0x10000130, 0x12018, ["main_body"]), (0x10000400, 0, ["asm_stub"])])
        self.assertEqual(kb.symbol_name(kb.symbol_at(
            table, 0x10000140)[1], {}), "main_body")
        self.assertEqual(kb.symbol_at(table, 0x10000401)[1], ["asm_stub"])

    def test_the_table_is_read_mangled_and_only_hit_bodies_are_demangled(self):
        asked = []
        nm_out = ("10000100 00000004 T _ZNKSt8optionalItE9has_valueEv\n"
                  "10000100 00000004 t _ZNK6FPBitsIfE6is_negEv\n"
                  "10000200 00000010 T _Z5outerv\n"
                  "10000300 00000010 T _Z9never_hitv\n")
        original = kb.nm
        kb.nm = lambda tree, demangle=True: asked.append(demangle) or nm_out
        try:
            table = kb.code_symbols(None)
        finally:
            kb.nm = original
        self.assertEqual(asked, [False])
        short = kb.symbol_names(table, [0x10000102, 0x10000204])
        self.assertNotIn("_Z9never_hitv", short)
        self.assertEqual(short["_Z5outerv"], "outer()")
        ranked = kb.rank_by_symbol(
            {0x10000102: 1, 0x10000204: 3}, table, top=5, short=short)
        self.assertEqual(ranked[0], (75.0, "outer()"))
        self.assertIn("FPBits::is_neg()", ranked[1][1])
        self.assertIn("[2 folded]", ranked[1][1])


class PanicRecord(unittest.TestCase):
    """`crash` decodes Kvasir::Panic::lastPanic: its magic and cause names must be Panic.hpp's."""

    def test_matches_panic_header(self):
        import re
        header = (HERE.parent / "src/kvasir/Util/Panic.hpp").read_text()
        magic = re.search(r"Magic = (0x[0-9A-Fa-f']+)U", header)
        self.assertIsNotNone(magic)
        self.assertEqual(
            int(magic.group(1).replace("'", ""), 16), kb.PANIC_MAGIC)
        body = re.search(
            r"enum class Cause : unsigned char \{(.*?)\};", header, re.S).group(1)
        values = [int(v) for v in re.findall(r"=\s*(\d+)", body)]
        self.assertEqual(values, list(range(len(values))),
                         "consecutive from 0")
        self.assertEqual(len(values), len(kb.PANIC_CAUSES))


class FullRecords(unittest.TestCase):
    """`crash` decodes the CrashRecord::Full records: Persistent's layout and the field lists must be
    the headers'."""

    @staticmethod
    def record(tag: str, version: int, values: list[int]) -> bytes:
        import zlib
        words = [kb.persistent_id(tag), version | (
            4 * len(values)) << 16] + values
        raw = b"".join(w.to_bytes(4, "little") for w in words)
        return raw + zlib.crc32(raw).to_bytes(4, "little")

    def test_ids_match_persistent_hpp(self):
        # tests/persistent_tests.cpp asserts the same word for "KFLT"
        self.assertEqual(kb.persistent_id("KFLT"), 0x544C464B)

    def test_round_trip_and_every_bad_bit(self):
        raw = self.record("KFLT", 2, list(range(33)))
        self.assertEqual(kb.decode_persistent(raw, "KFLT", 2), list(range(33)))
        self.assertIsNone(kb.decode_persistent(raw, "KFLT", 1))
        self.assertIsNone(kb.decode_persistent(raw, "KPNC", 2))
        for bit in range(len(raw) * 8):
            bad = bytearray(raw)
            bad[bit // 8] ^= 1 << (bit % 8)
            self.assertIsNone(kb.decode_persistent(bytes(bad), "KFLT", 2), bit)

    def test_fields_match_the_headers(self):
        import re
        header = (HERE.parent / "src/kvasir/Util/FaultHandler.hpp").read_text()
        body = re.search(
            r"struct FullRecord \{.*?\n    \};", header, re.S).group(0)
        names = []
        for line in body.splitlines():
            m = re.match(r"\s*std::uint32_t ([^;]+);", line)
            if m:
                names += [n.strip() for n in m.group(1).split(",")]
        translate = {"excReturn": "exc_return"}
        self.assertEqual([translate.get(n, n)
                         for n in names], kb.FAULT_V2_FIELDS)
        self.assertIn("sizeof(FullRecord) == 33 * 4", header)
        panic = (HERE.parent / "src/kvasir/Util/Panic.hpp").read_text()
        body = re.search(r"struct FullRecord \{(.*?)\};", panic, re.S).group(1)
        self.assertEqual(re.findall(
            r"std::uint32_t (\w+);", body), kb.PANIC_V2_FIELDS)

    def test_cfsr_names(self):
        self.assertEqual(kb.cfsr_text(0x00008200), "PRECISERR BFARVALID")
        self.assertEqual(kb.cfsr_text(0x00100000), "STKOF")
        self.assertEqual(kb.cfsr_text(0), "-")


class PanicCommand(unittest.TestCase):
    """`panic` makes the core call Kvasir::Panic::raise(cause) through the debug registers."""

    def test_causes_follow_the_enum(self):
        causes = kb.raise_causes()
        self.assertEqual(causes["assertion"], 0)
        self.assertEqual(causes["fault"], 7)
        self.assertEqual(causes["user"], 8, "KVASIR_PANIC is Cause::user")
        self.assertEqual(sorted(causes.values()),
                         list(range(len(kb.PANIC_CAUSES))))

    def test_raise_is_the_header_s_signature(self):
        header = (HERE.parent / "src/kvasir/Util/Panic.hpp").read_text()
        self.assertIn("[[noreturn]] void raise(Cause cause);", header)
        self.assertEqual(
            kb.PANIC_RAISE, "Kvasir::Panic::raise(Kvasir::Panic::Cause)")

    def test_register_writes_r0_xpsr_without_it_ici_then_pc(self):
        # a halt inside an IT block of an exception handler: IT/ICI bits set, IPSR 3
        steps = kb.panic_register_writes(0x20026439, 8, 0x89002803, 0x10001234)
        self.assertEqual(steps[0], [(kb.DCRDR, 8), (kb.DCRSR, 0x10000)])
        self.assertEqual(steps[1], [(kb.DCRDR, 0x10001237), (kb.DCRSR, 0x1000E)],
                         "LR = a call from where the core stood: the record's site is that PC")
        self.assertEqual(steps[2], [(kb.DCRDR, 0x89000003), (kb.DCRSR, 0x10010)],
                         "IT/ICI cleared (DDI0553B.y RQLRN), flags, T and IPSR kept")
        self.assertEqual(steps[3], [(kb.DCRDR, 0x20026438), (kb.DCRSR, 0x1000F)],
                         "the PC without the Thumb bit")

    def test_it_ici_mask(self):
        self.assertEqual(kb.EPSR_IT_ICI, 0x0600FC00)

    def test_gdb_commands(self):
        self.assertEqual(kb.panic_gdb_commands(0x20026439, 3),
                         ["monitor halt", "set $r0 = 3", "set $lr = ($pc + 2) | 1",
                          "set $xpsr = $xpsr & ~0x600fc00",
                          "set $pc = 0x20026438", "monitor go", "detach"])


class TreeSettings(unittest.TestCase):
    def tree(self, cache):
        tree = kb.Tree.__new__(kb.Tree)
        tree.cache = cache
        return tree

    def test_the_environment_overrides_the_cache_even_when_empty(self):
        tree = self.tree("JLINK_IP:STRING=192.168.4.180\n")
        with unittest.mock.patch.dict(kb.os.environ, {"JLINK_IP": ""}):
            self.assertEqual(tree._setting("JLINK_IP"), "")
        with unittest.mock.patch.dict(kb.os.environ, {}, clear=True):
            self.assertEqual(tree._setting("JLINK_IP"), "192.168.4.180")


class RamImage(unittest.TestCase):
    """A RAM_ONLY image goes through a printer only when that one starts it (--ram_image)."""

    def tree(self, build, ninja_command=None, script=None):
        tree = kb.Tree.__new__(kb.Tree)
        tree.build, tree.target = Path(build), "t"
        if ninja_command is not None:
            Path(build, "build.ninja").write_text(
                "# Custom command for CMakeFiles/log_t\n"
                "build CMakeFiles/log_t | ${cmake_ninja_workdir}CMakeFiles/log_t: CUSTOM_COMMAND\n"
                f"  COMMAND = {ninja_command}\n")
        if script is not None:
            Path(build, "t_flash.jlink").write_text(script)
        return tree

    def test_the_log_command_or_the_jlink_script_tells(self):
        with tempfile.TemporaryDirectory() as b:
            self.assertFalse(self.tree(b, "cd /x && printer --device D --hex_file t.hex",
                                       "loadfile t.hex\nr\nh\nr\ng\nq").ram_image())
        with tempfile.TemporaryDirectory() as b:
            tree = self.tree(
                b, "cd /x && printer --device D --ram_image --log_filter f.txt")
            self.assertTrue(tree.ram_image())
            self.assertEqual(tree._log_command_values(
                "--log_filter"), ["f.txt"])
        with tempfile.TemporaryDirectory() as b:
            self.assertTrue(self.tree(b, None, "loadfile t.hex\nwreg MSP 0x20082000\n"
                                      "SetPC 0x20000198\ng\nq").ram_image())

    def guard(self, status, ram_image=True):
        tree = type("Tree", (), {"ram_image": lambda self: ram_image,
                                 "build": "b", "target": "t"})()
        err = io.StringIO()
        with unittest.mock.patch.object(kb, "request", lambda control, req: status), \
                contextlib.redirect_stderr(err):
            try:
                kb.need_ram_image_printer(tree, Path("control.sock"), "flash")
            except SystemExit as e:
                return e.code, err.getvalue()
        return 0, err.getvalue()

    def test_a_printer_that_starts_ram_images_is_used(self):
        self.assertEqual(self.guard({"ram_image": True}), (0, ""))

    def test_an_ordinary_image_needs_nothing(self):
        self.assertEqual(self.guard({}, ram_image=False), (0, ""))

    def test_an_old_printer_is_refused(self):
        code, text = self.guard({"running": True})
        self.assertEqual(code, 4)
        self.assertIn("from before --ram_image", text)

    def test_a_printer_started_without_the_flag_is_refused(self):
        code, text = self.guard({"ram_image": False})
        self.assertEqual(code, 4)
        self.assertIn("started without --ram_image", text)


class ChipPlugin(unittest.TestCase):
    def tree(self, chip_root):
        return type("Tree", (), {"chip_root": chip_root})()

    def listing(self, chip_root):
        out = io.StringIO()
        original = kb.Tree
        kb.Tree = lambda build, target: self.tree(chip_root)
        try:
            with contextlib.redirect_stdout(out):
                kb.chip_cmd(
                    type("Args", (), {"build": "b", "target": "t", "rest": []})())
        finally:
            kb.Tree = original
        return out.getvalue()

    def test_a_chips_commands_are_listed(self):
        with tempfile.TemporaryDirectory() as root:
            Path(root, "tools").mkdir()
            Path(root, "tools", "kvasir_bench_chip.py").write_text(
                "def commands():\n    return {'xyz': ('does xyz', lambda p: None, lambda b, a: None)}\n")
            self.assertIsNotNone(kb.load_chip_plugin(self.tree(root)))
            self.assertIn("xyz        does xyz", self.listing(root))

    def test_no_file_no_commands(self):
        with tempfile.TemporaryDirectory() as root:
            self.assertIsNone(kb.load_chip_plugin(self.tree(root)))
            self.assertIn("no chip commands", self.listing(root))
        self.assertIsNone(kb.load_chip_plugin(self.tree("")))


class TraceRing(unittest.TestCase):
    """Both layouts of a Kvasir::Trace::Ring: one RAM object with the header in front (the old
    layout), and a constant descriptor plus RAM storage."""
    RECORDS = b"".join(v.to_bytes(4, "little")
                       for v in (10, 11, 20, 21, 30, 31, 40, 41))

    def test_descriptor_and_storage(self):
        descriptor = (kb.TRACE_MAGIC.to_bytes(4, "little") + (2).to_bytes(2, "little")
                      + (4).to_bytes(2, "little") + (0x10001234).to_bytes(4, "little"))
        storage = (5).to_bytes(4, "little") + self.RECORDS
        self.assertEqual(kb.trace_layout(descriptor, storage),
                         (2, 4, 5, 0x10001234, 0, 4))

    def test_old_layout(self):
        storage = (kb.TRACE_MAGIC.to_bytes(4, "little") + (2).to_bytes(2, "little")
                   + (4).to_bytes(2, "little") + (5).to_bytes(4, "little")
                   + (0x10001234).to_bytes(4, "little") + self.RECORDS)
        self.assertEqual(kb.trace_layout(None, storage),
                         (2, 4, 5, 0x10001234, 8, 16))

    def test_not_a_ring(self):
        self.assertIsNone(kb.trace_layout(None, bytes(48))
                          )                    # no magic
        descriptor = (kb.TRACE_MAGIC.to_bytes(4, "little") + (2).to_bytes(2, "little")
                      + (64).to_bytes(2, "little") + bytes(4))
        self.assertIsNone(kb.trace_layout(descriptor, bytes(36))
                          )              # storage too short

    def test_rings_pair_descriptor_with_storage(self):
        new = 'Kvasir::Trace::Ring<Kvasir::Trace::Name<4u>{"usb"}, 64u>::'
        old = 'Kvasir::Trace::Ring<Kvasir::Trace::Name<5u>{"loop"}, 16u>::'
        all_symbols = [(0x10002000, 12, new + "descriptor"), (0x20000100, 516, new + "storage"),
                       (0x20000400, 208, old + "storage"), (0x20000800, 4, "other::storage")]
        self.assertEqual(kb.trace_rings(all_symbols),
                         [(0x10002000, 0x20000100, 516), (None, 0x20000400, 208)])


if __name__ == "__main__":
    unittest.main()
