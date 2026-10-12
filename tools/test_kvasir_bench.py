#!/usr/bin/env python3
"""kvasir_bench.py's parts that need no board: profile ranking, chip commands."""
import contextlib
import importlib.util
import io
import json
import os
import shutil
import socket
import sys
import tempfile
import threading
import types
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


class WaitForDropped(unittest.TestCase):
    """A printer that dropped lines for this connection: wait-for fails (exit 5) instead of waiting."""

    def test_a_dropped_event_ends_the_wait_with_exit_5(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "control.sock"
            server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            server.bind(str(path))
            server.listen(1)

            def printer():
                conn, _ = server.accept()
                with conn:
                    conn.recv(65536)   # the subscribe
                    log = {"cmd": "log", "recv_time": "t", "channel": 0, "file": "f.cpp", "line": 1,
                           "function": "f", "level": "info", "uc_time_ns": 1, "message": "other",
                           "module": "", "seq": 0}
                    lines = [{"cmd": "subscribe", "protocol": kb.PROTOCOL}, log,
                             {"cmd": "dropped", "lines": 7}]
                    conn.sendall(
                        "".join(json.dumps(x) + "\n" for x in lines).encode())
                    conn.recv(1)   # until the client closes

            thread = threading.Thread(target=printer, daemon=True)
            thread.start()
            args = types.SimpleNamespace(build="b", target="t", regex="never", timeout=5.0,
                                         from_now=True, from_start=False, module=None,
                                         not_module=None, width=200, last=5)
            out = io.StringIO()
            err = io.StringIO()
            with unittest.mock.patch.object(kb, "Tree"), \
                    unittest.mock.patch.object(kb, "need_printer", return_value=path), \
                    contextlib.redirect_stdout(out), contextlib.redirect_stderr(err), \
                    self.assertRaises(SystemExit) as exit_:
                kb.wait_for(args)
            self.assertEqual(exit_.exception.code, 5)
            self.assertIn("7 lines dropped for this client", out.getvalue())
            # a caller acts only after this line (scenario harnesses: no race with what it waits for)
            self.assertEqual(err.getvalue().splitlines()
                             [0], "wait-for: subscribed")
            server.close()


class PrinterGdbStub(unittest.TestCase):
    """snapshot under a printer: gdb's remote protocol answered from the printer's registers and reads."""

    def exchange(self, packets: list[str]) -> list[str]:
        ours, theirs = socket.socketpair()
        memory = {0x20000000 + i: i & 0xFF for i in range(64)}
        regs = list(range(0x100, 0x111))
        thread = threading.Thread(target=kb.serve_gdb, args=(
            theirs, regs,
            lambda a, n: bytes(memory[a + i] for i in range(n)) if all(a + i in memory for i in range(n))
            else None))
        thread.start()
        replies = []
        with ours:
            for p in packets:
                ours.sendall(kb.gdb_packet(p))
                data = b""
                while not (data.count(b"#") and len(data) >= data.rindex(b"#") + 3):
                    data += ours.recv(65536)
                body = data[data.index(b"$") + 1:data.rindex(b"#")].decode()
                self.assertEqual(data[:1], b"+", "every packet acknowledged")
                replies.append(body)
        thread.join(timeout=5)
        theirs.close()
        return replies

    def test_registers_memory_target_description_and_detach(self):
        g, p15, p16, m, bad, xml, d = self.exchange(
            ["g", "pf", "p10", "m20000004,4", "m30000000,4", "qXfer:features:read:target.xml:0,fff", "D"])
        self.assertEqual(len(g), 17 * 8)
        self.assertEqual(g[:8], "00010000")
        self.assertEqual(p15, (0x10F).to_bytes(4, "little").hex(), "pc")
        self.assertEqual(p16, (0x110).to_bytes(4, "little").hex(), "xpsr")
        self.assertEqual(m, "04050607")
        self.assertEqual(
            bad, "E01", "an unreadable address is an error, not zeros")
        self.assertTrue(xml.startswith("l<?xml")
                        and "org.gnu.gdb.arm.m-profile" in xml)
        self.assertEqual(d, "OK")


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
        enumerators = re.findall(r"(\w+)\s*=\s*(\d+)", body)
        values = [int(v) for _, v in enumerators]
        self.assertEqual(values, list(range(len(values))),
                         "consecutive from 0")
        names = dict(re.findall(
            r'case Cause::(\w+):\s*return "([^"]*)";', header))
        self.assertEqual([names.get(e) for e, _ in enumerators], kb.PANIC_CAUSES,
                         "name(), in Cause order")


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
        writes = kb.panic_register_writes(
            0x20026439, 8, 0x89002803, 0x10001234)
        self.assertEqual(writes[0], (0, 8), "R0 = the cause")
        self.assertEqual(writes[1], (14, 0x10001237),
                         "LR = a call from where the core stood: the record's site is that PC")
        self.assertEqual(writes[2], (16, 0x89000003),
                         "IT/ICI cleared (DDI0553B.y RQLRN), flags, T and IPSR kept")
        self.assertEqual(writes[3], (15, 0x20026438),
                         "the PC without the Thumb bit")
        self.assertEqual(kb.REGISTER_NAMES[0], "r0")
        self.assertEqual([kb.REGISTER_NAMES[i] for i in (14, 15, 16)], ["lr", "pc", "xpsr"],
                         "the indices are the printer's (uc_log_client REGISTER_NAMES)")

    def test_it_ici_mask(self):
        self.assertEqual(kb.EPSR_IT_ICI, 0x0600FC00)


class TreeSettings(unittest.TestCase):
    """The target's printer options come from the tree's <target>.printer_args, one argument a line."""

    def tree(self, build, cache="", args=("--device", "D")):
        tree = kb.Tree.__new__(kb.Tree)
        tree.build, tree.target, tree.cache = Path(build), "t", cache
        if args is not None:
            Path(build, "t.printer_args").write_text(
                "".join(f"{a}\n" for a in args))
        return tree

    def test_the_environment_overrides_the_file_even_when_empty(self):
        with tempfile.TemporaryDirectory() as b:
            tree = self.tree(b, args=("--device", "D", "--host", "192.0.2.10"))
            with unittest.mock.patch.dict(kb.os.environ, {"JLINK_IP": ""}):
                self.assertEqual(tree._setting("JLINK_IP"), "")
            with unittest.mock.patch.dict(kb.os.environ, {}, clear=True):
                self.assertEqual(tree._setting("JLINK_IP"), "192.0.2.10")
                self.assertEqual(tree._setting("JLINK_PROBE"), "",
                                 "options without --probe name no USB probe")

    def test_one_argument_a_line_values_with_spaces_and_empty_ones(self):
        with tempfile.TemporaryDirectory() as b:
            tree = self.tree(b, args=("--device", "RP2350_M33_0", "--speed", "4000", "--probe", "Base Probe",
                                      "--stall_recovery", "w4 0x1 0x2", "--stall_recovery", "w4 0x3 0x4",
                                      "--log_filter", ""))
            self.assertEqual(tree._device(), "RP2350_M33_0")
            self.assertEqual(tree._arg_values("--speed"), ["4000"])
            self.assertEqual(tree._arg_values(
                "--stall_recovery"), ["w4 0x1 0x2", "w4 0x3 0x4"])
            self.assertEqual(tree._arg_values("--log_filter"), [""])
            with unittest.mock.patch.dict(kb.os.environ, {}, clear=True):
                self.assertEqual(tree._setting("JLINK_PROBE"), "Base Probe")

    def test_the_printer_command_is_the_file_with_the_bench_s_own_probe_and_log(self):
        with tempfile.TemporaryDirectory() as b:
            tree = self.tree(b, args=("--device", "D", "--speed", "4000", "--probe", "Old",
                                      "--pre_reset_command", "w4 0x5 0x6", "--ram_image"))
            tree.log_dir = Path(b, "log")
            tree.host, tree.probe = "", "New"
            with unittest.mock.patch.dict(kb.os.environ, {}, clear=True):
                cmd = tree.printer_command(Path("/p"))
            self.assertEqual(cmd, ["/p", "--device", "D", "--speed", "4000", "--pre_reset_command", "w4 0x5 0x6",
                                   "--ram_image", "--probe", "New", "--build_command", "true",
                                   "--log_dir", str(Path(b, "log")), "--disable_ui"])
            tree.host = "192.0.2.30"
            with unittest.mock.patch.dict(kb.os.environ, {"JLINK_DEVICE": "E"}, clear=True):
                cmd = tree.printer_command(Path("/p"))
            self.assertEqual(cmd[:3], ["/p", "--device", "E"],
                             "JLINK_DEVICE replaces the file's device")
            self.assertIn("192.0.2.30", cmd)
            self.assertNotIn(
                "New", cmd, "a network J-Link instead of the USB probe")

    def test_a_tree_without_the_file_is_said(self):
        with tempfile.TemporaryDirectory() as b:
            tree = self.tree(b, args=None)
            with contextlib.redirect_stderr(io.StringIO()) as err, self.assertRaises(SystemExit):
                tree.printer_args()
            self.assertIn(
                f"no t.printer_args: run cmake {b} once", err.getvalue())


class PrinterBinaryOverride(unittest.TestCase):
    """UC_LOG_PRINTER_BIN: that printer is started as it is - the tree's printer is not built."""

    def test_the_override_is_started_and_not_built(self):
        with tempfile.TemporaryDirectory() as b:
            tree = TreeSettings.tree(None, b)
            tree.device, tree.swd_speed, tree.host, tree.probe = "D", "4000", "", ""
            tree.log_dir = Path(b, "log")
            runs = []

            def run(cmd, **_kw):
                runs.append(cmd)
                return kb.subprocess.CompletedProcess(cmd, 0, "", "")

            started = []

            class Proc:
                pid = 7

                def __init__(self, cmd, **_kw):
                    started.append(cmd)

                def poll(self):
                    return None

            with unittest.mock.patch.dict(kb.os.environ, {"UC_LOG_PRINTER_BIN": sys.executable}), \
                    unittest.mock.patch.object(kb.subprocess, "run", run), \
                    unittest.mock.patch.object(kb.subprocess, "Popen", Proc), \
                    unittest.mock.patch.object(kb, "control_socket", return_value=None), \
                    unittest.mock.patch.object(kb, "printers", return_value=[]), \
                    unittest.mock.patch.object(kb.time, "sleep"):
                self.assertEqual(kb.start_printer(tree), 7)
            self.assertFalse([c for c in runs if "cmake" in c],
                             "the tree's printer is not built")
            self.assertEqual(started[0][0], sys.executable,
                             "the override is the printer started")


class PrinterStop(unittest.TestCase):
    """`printer stop` returns once the printer is gone (and with it its hold on the probe); one that does not go is an
    error, not a success."""

    def run_stop(self, gone_after: int | None):
        with tempfile.TemporaryDirectory() as b:
            tree = TreeSettings.tree(None, b)
            tree.log_dir = Path(b, "log")
            looks = {"n": 0}

            def exists(path):
                if not str(path).startswith("/proc/4242"):
                    return os.path.exists(path)
                looks["n"] += 1
                return gone_after is None or looks["n"] <= gone_after
            clock = iter(range(0, 1000))
            args = types.SimpleNamespace(build=b, target="t", confirm=False)
            with unittest.mock.patch.object(kb, "Tree", return_value=tree), \
                    unittest.mock.patch.object(kb, "printers", return_value=[(4242, f"p --disable_ui --log_dir {tree.log_dir}", 1.0)]), \
                    unittest.mock.patch.object(kb.os, "kill") as kill, \
                    unittest.mock.patch.object(kb.Path, "exists", lambda self: exists(self)), \
                    unittest.mock.patch.object(kb.time, "sleep"), \
                    unittest.mock.patch.object(kb.time, "monotonic", lambda: next(clock) * 0.05), \
                    contextlib.redirect_stdout(io.StringIO()) as out, contextlib.redirect_stderr(io.StringIO()) as err:
                try:
                    kb.printer_stop(args)
                    code = 0
                except SystemExit as e:
                    code = e.code
            kill.assert_called_once()
            return code, out.getvalue() + err.getvalue(), looks["n"]

    def test_waits_until_gone(self):
        code, text, looks = self.run_stop(gone_after=5)
        self.assertEqual(code, 0)
        self.assertIn("stopped printer 4242", text)
        self.assertGreater(looks, 5)

    def test_one_that_stays_is_an_error(self):
        code, text, _ = self.run_stop(gone_after=None)
        self.assertNotEqual(code, 0)
        self.assertIn("still exiting", text)


class NoSecondPrinter(unittest.TestCase):
    """A command never starts a printer beside one that serves the target but does not answer yet: it waits for that
    one's socket and uses it, or refuses."""

    def tree(self, b):
        tree = TreeSettings.tree(None, b)
        tree.log_dir = Path(b, "log")
        return tree

    def test_a_starting_printer_is_waited_for_and_used(self):
        with tempfile.TemporaryDirectory() as b:
            tree = self.tree(b)
            answers = iter([None, None, Path(b, "sock")])
            with unittest.mock.patch.object(kb, "control_socket", lambda t: next(answers)), \
                    unittest.mock.patch.object(kb, "printers", return_value=[(os.getpid(), f"p --log_dir {tree.log_dir}", 1.0)]), \
                    unittest.mock.patch.object(kb, "start_printer") as start, \
                    unittest.mock.patch.object(kb.time, "sleep"):
                self.assertEqual(kb.need_printer(tree, "t"), Path(b, "sock"))
                start.assert_not_called()

    def test_one_that_never_answers_is_refused_not_doubled(self):
        with tempfile.TemporaryDirectory() as b:
            tree = self.tree(b)
            clock = iter(range(0, 100))
            with unittest.mock.patch.object(kb, "control_socket", return_value=None), \
                    unittest.mock.patch.object(kb, "printers", return_value=[(os.getpid(), f"p --log_dir {tree.log_dir}", 1.0)]), \
                    unittest.mock.patch.object(kb, "start_printer") as start, \
                    unittest.mock.patch.object(kb.time, "sleep"), \
                    unittest.mock.patch.object(kb.time, "monotonic", lambda: next(clock)), \
                    contextlib.redirect_stderr(io.StringIO()) as err, self.assertRaises(SystemExit) as e:
                kb.need_printer(tree, "t")
            start.assert_not_called()
            self.assertEqual(e.exception.code, 4)
            self.assertIn("not starting a second one", err.getvalue())


class ShortPath(unittest.TestCase):
    def tree(self, cache):
        with tempfile.TemporaryDirectory() as b:
            Path(b, "CMakeCache.txt").write_text(cache)
            Path(b, "t.elf").touch()
            Path(b, "t.printer_args").write_text("--device\nD\n")
            return kb.Tree(b, "t")

    def test_paths_are_shown_from_the_directory_above_the_source(self):
        tree = self.tree("CMAKE_HOME_DIRECTORY:INTERNAL=/src/work/firmware\n")
        self.assertEqual(tree.short_path(
            "/src/work/firmware/src/main.cpp"), "firmware/src/main.cpp")
        self.assertEqual(tree.short_path(
            "/src/work/sdk/src/a.hpp"), "sdk/src/a.hpp")
        self.assertEqual(tree.short_path(
            "/usr/include/c++/v1/vector"), "/usr/include/c++/v1/vector")
        self.assertEqual(tree.short_path(
            "/src/workshop/x.cpp"), "/src/workshop/x.cpp")

    def test_without_a_source_directory_the_path_stays_whole(self):
        self.assertEqual(self.tree("").short_path("/a/b.cpp"), "/a/b.cpp")


class RamImage(unittest.TestCase):
    """A RAM_ONLY image goes through a printer only when that one starts it (--ram_image)."""

    def test_the_printer_options_tell(self):
        with tempfile.TemporaryDirectory() as b:
            self.assertFalse(TreeSettings.tree(None, b, args=(
                "--device", "D", "--hex_file", "t.hex")).ram_image())
        with tempfile.TemporaryDirectory() as b:
            self.assertTrue(TreeSettings.tree(None, b, args=(
                "--device", "D", "--ram_image")).ram_image())

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


class CoverageManifest(unittest.TestCase):
    """assemble_profraw: Kvasir::Coverage's manifest (12-byte pieces on the target) into the .profraw bytes."""

    @staticmethod
    def manifest(pieces, header, magic=kb.COVERAGE_MAGIC, status=0):
        import struct
        raw = bytearray(struct.pack(
            "<4I", magic, status, len(pieces), len(header)))
        for p in pieces:
            raw += struct.pack("<3I", *p)
        raw += bytes(16 + 12 * kb.COVERAGE_PIECES - len(raw))
        raw += bytes(((len(raw) + 7) & ~7) - len(raw)) + header
        return bytes(raw)

    def test_pieces_in_order(self):
        flash = {0x10001000: b"RECORDS!", 0x10002000: b"names"}
        ram = {0x20000100: b"\x00\xff\x00"}
        raw = self.manifest([(0, 4, 2), (0x10001000, 8, 0), (0, 2, 1), (0x20000100, 3, 0), (0x10002000, 5, 0),
                             (4, 2, 2)], b"HEADxy")
        reads = []

        def live(address, length):
            reads.append(address)
            return ram[address][:length]

        out = kb.assemble_profraw(
            raw, lambda a, n: flash.get(a, None) and flash[a][:n], live)
        self.assertEqual(out, b"HEAD" + b"RECORDS!" +
                         b"\0\0" + b"\x00\xff\x00" + b"names" + b"xy")
        # only the counters come from the board
        self.assertEqual(reads, [0x20000100])

    def test_refuses_an_empty_or_failed_manifest(self):
        with self.assertRaisesRegex(ValueError, "not run Kvasir::Coverage"):
            kb.assemble_profraw(self.manifest([], b"", magic=0),
                                lambda a, n: None, lambda a, n: b"")
        with self.assertRaisesRegex(ValueError, "reported 1"):
            kb.assemble_profraw(self.manifest(
                [], b"", status=1), lambda a, n: None, lambda a, n: b"")


def tiny_elf(sections):
    """A little-endian ELF32 image: `sections` are (name, sh_flags, address, load address, bytes), each in a PT_LOAD
    segment of its own, followed by .shstrtab."""
    import struct
    names = b"\0"
    name_at = []
    for (name, *_rest) in sections:
        name_at.append(len(names))
        names += name.encode() + b"\0"
    shstrtab_name = len(names)
    names += b".shstrtab\0"
    phoff = 52
    data_at = phoff + 32 * len(sections)
    blob, offsets = b"", []
    for (*_rest, data) in sections:
        offsets.append(data_at + len(blob))
        blob += data + bytes(-len(data) % 4)
    names_at = data_at + len(blob)
    shoff = names_at + len(names) + (-len(names) % 4)
    shnum = len(sections) + 2
    out = bytearray(b"\x7fELF\x01\x01\x01" + bytes(9))
    out += struct.pack("<HHIIIIIHHHHHH", 2, 40, 1, 0, phoff,
                       shoff, 0, 52, 32, len(sections), 40, shnum, shnum - 1)
    for (_name, _flags, address, lma, data), at in zip(sections, offsets):
        out += struct.pack("<8I", 1, at, address, lma,
                           len(data), len(data), 5, 4)
    out += blob + names + bytes(shoff - names_at - len(names))
    out += bytes(40)
    for (_name, flags, address, _lma, data), at, n in zip(sections, offsets, name_at):
        out += struct.pack("<10I", n, 1, flags, address,
                           at, len(data), 0, 0, 4, 0)
    out += struct.pack("<10I", shstrtab_name, 3, 0, 0,
                       names_at, len(names), 0, 0, 1, 0)
    return bytes(out)


class CoverageImageOrBoard(unittest.TestCase):
    """coverage takes a piece from the ELF only where the board holds exactly the ELF's bytes: a section that runs
    where it is loaded and is not writable. Decided from the section, never from its address."""

    @staticmethod
    def reader(sections):
        sys.path.insert(
            0, str(Path(__file__).resolve().parent.parent / "cmake" / "tools"))
        import elf_image
        return kb.image_reader(elf_image.Elf(tiny_elf(sections)).resident())

    ALLOC, WRITE = 0x2, 0x1

    def test_records_in_flash_come_from_the_image(self):
        read = self.reader(
            [("__llvm_prf_data", self.ALLOC, 0x10001000, 0x10001000, b"RECORDS!")])
        self.assertEqual(read(0x10001002, 4), b"CORD")

    def test_counters_copied_to_ram_are_read_live(self):
        # .data: runs in RAM, loaded from flash, writable - neither address is the image's to answer
        read = self.reader(
            [(".data", self.ALLOC | self.WRITE, 0x20000100, 0x10002000, b"\0\0\0\0")])
        self.assertIsNone(read(0x20000100, 4))
        self.assertIsNone(read(0x10002000, 4))

    def test_writable_memory_below_0x20000000_is_read_live(self):
        # RAM at a low address (tightly coupled memory): the old `lma < 0x20000000` took it from the image
        read = self.reader(
            [(".dtcm", self.ALLOC | self.WRITE, 0x00000400, 0x00000400, b"live")])
        self.assertIsNone(read(0x00000400, 4))

    def test_read_only_section_above_0x20000000_comes_from_the_image(self):
        # flash at a high address: the old check sent it to the board
        read = self.reader(
            [("__llvm_prf_names", self.ALLOC, 0x30000000, 0x30000000, b"names")])
        self.assertEqual(read(0x30000000, 5), b"names")

    def test_read_only_code_copied_to_ram_is_read_live(self):
        read = self.reader(
            [(".ramfunc", self.ALLOC, 0x20000000, 0x10003000, b"code")])
        self.assertIsNone(read(0x20000000, 4))

    def test_a_piece_past_the_section_end_is_read_live(self):
        read = self.reader(
            [("__llvm_prf_data", self.ALLOC, 0x10001000, 0x10001000, b"RECORDS!")])
        self.assertIsNone(read(0x10001004, 8))


class PrinterOnly(unittest.TestCase):
    """kvasir_bench never opens a probe itself: no J-Link tool, GDB server or J-Link DLL anywhere
    in it - every command goes through the printer that holds the probe."""

    def test_no_jlink_tool_or_library_is_named(self):
        source = (HERE / "kvasir_bench.py").read_text()
        code = "\n".join(l for l in source.splitlines()
                         if not l.lstrip().startswith("#"))
        for forbidden in ("JLinkExe", "JLinkGDBServer", "JLinkRTT", "libjlinkarm", "pylink",
                          "ctypes", "-CommandFile"):
            self.assertNotIn(
                forbidden, code, f"kvasir_bench names {forbidden}")

    def test_probes_come_from_sysfs(self):
        with tempfile.TemporaryDirectory() as d:
            for name, vendor, serial, product in (("3-1", "1366", "000012345678", "J-Link_Bench One"),
                                                  ("3-2", "1366",
                                                   "000087654321", "J-Link"),
                                                  ("1-4", "046d", "x", "Mouse")):
                dev = Path(d) / name
                dev.mkdir()
                (dev / "idVendor").write_text(vendor + "\n")
                (dev / "serial").write_text(serial + "\n")
                (dev / "product").write_text(product + "\n")
            self.assertEqual(kb.probes_on_usb(Path(d)),
                             [("12345678", "J-Link Bench One", "Bench One"), ("87654321", "J-Link", "")])


class HealthRecords(unittest.TestCase):
    """crash's view of Health's KHLT/KHRV and the boot guard's KBTG records."""

    @staticmethod
    def record(tag, values):
        import zlib
        words = [kb.persistent_id(tag), 1 | (4 * len(values)) << 16] + values
        raw = b"".join(w.to_bytes(4, "little") for w in words)
        return raw + zlib.crc32(raw).to_bytes(4, "little")

    @staticmethod
    def guard(boot, consecutive, total, failure, detail, flags):
        import zlib
        value = (boot.to_bytes(4, "little") + consecutive.to_bytes(2, "little") + total.to_bytes(2, "little")
                 + bytes([failure, detail, flags, 0]))
        raw = kb.persistent_id("KBTG").to_bytes(
            4, "little") + (1 | 12 << 16).to_bytes(4, "little") + value
        return raw + zlib.crc32(raw).to_bytes(4, "little")

    TABLE = [(0x2000_0000, 36, "Kvasir::Health::lastStarvation"),
             (0x2000_0100, 36, "Kvasir::Health::lastRecovery"),
             (0x2000_0200, 24, "Kvasir::BootGuard::state")]
    RESIDENT = [
        (".text", 0x1000_0000, b"\x00\x01tick\x00engine\x00core1\x00\xffxled\x00")]

    def lines(self, memory, table=None):
        def read(where, size):
            return memory.get(where, bytes(size))[:size].ljust(size, b"\x00")
        return kb.health_lines(table or self.TABLE, read, self.RESIDENT)

    def test_fields_match_the_headers(self):
        import re
        header = (HERE.parent / "src/kvasir/Util/HealthRecords.hpp").read_text()
        for struct, fields in [("Starvation", kb.HEALTH_STARVATION_FIELDS),
                               ("Recoveries", kb.HEALTH_RECOVERY_FIELDS)]:
            body = re.search(
                rf"struct {struct} \{{(.*?)\}};", header, re.S).group(1)
            self.assertEqual(re.findall(
                r"std::uint32_t (\w+);", body), fields, struct)
        self.assertIn('persistentId("KHLT")', header)
        self.assertIn('persistentId("KHRV")', header)
        self.assertIn("(check + 1) | (attempt << 16U)", header)
        rules = (HERE.parent / "src/kvasir/Util/BootGuardRules.hpp").read_text()
        failures = re.search(
            r"enum class Failure : std::uint8_t \{([^}]*)\}", rules).group(1)
        self.assertEqual([f.strip()
                         for f in failures.split(",")], kb.BOOT_GUARD_FAILURES)
        flags = re.findall(r"(\w+)\s*=\s*(\d+),", re.search(r"enum Flag : std::uint8_t \{(.*?)\};", rules,
                                                            re.S).group(1))
        self.assertEqual([n for n, _ in flags], kb.BOOT_GUARD_FLAGS)
        self.assertEqual([int(v) for _, v in flags], [
                         1 << i for i in range(len(flags))])
        self.assertIn('persistentId("KBTG")', rules)

    def test_fnv1a_is_the_header_s(self):
        # the FNV-1a reference values; HealthRecords.hpp's loop is the same offset basis and prime
        self.assertEqual(kb.fnv1a(b""), 0x811C9DC5)
        self.assertEqual(kb.fnv1a(b"a"), 0xE40C292C)
        self.assertEqual(kb.fnv1a(b"foobar"), 0xBF9CF968)
        header = (HERE.parent / "src/kvasir/Util/HealthRecords.hpp").read_text()
        self.assertIn("0x811C'9DC5U", header)
        self.assertIn("0x0100'0193U", header)

    def test_names_whole_strings_and_merged_tails(self):
        wanted = {kb.fnv1a(b"engine"), kb.fnv1a(b"led"), kb.fnv1a(b"nowhere")}
        self.assertEqual(kb.names_by_hash(self.RESIDENT, wanted),
                         {kb.fnv1a(b"engine"): "engine", kb.fnv1a(b"led"): "led"})

    def test_a_starvation(self):
        memory = {0x2000_0000: self.record(
            "KHLT", [1, kb.fnv1a(b"engine"), 812, 500, 4321, 0x6])}
        out = self.lines(memory)
        self.assertIn(
            "check 'engine' (#1) starved, silent 812 ms of the 500 ms allowed, 4321 ms after boot", out[0])
        self.assertIn("0x6", out[0])
        self.assertIn("lastRecovery) is empty", out[1])

    def test_a_reset_inside_on_starved_names_the_check_and_the_attempt(self):
        memory = {0x2000_0100: self.record("KHRV", [2, 3, kb.fnv1a(b"core1"), 9000, (3 + 1) | (2 << 16), 0]),
                  0x2000_0200: self.guard(7, 1, 4, 4, 3, 1 | 16)}
        out = "\n".join(self.lines(memory))
        self.assertIn(
            "2 recoveries this run, the last check 'core1' (#3) at 9000 ms", out)
        self.assertIn(
            "check 'core1' (#3)'s onStarved was running, attempt 2", out)
        self.assertIn("boot 7, 1 failed run(s) in a row, 4 in all, the last failure starvation "
                      "(check 'core1' (#3)); runOpen healthyThisRun", out)

    def test_an_index_without_a_hash_and_a_hash_not_in_the_image(self):
        memory = {0x2000_0000: self.record("KHLT", [0, kb.fnv1a(b"renamed"), 1, 1, 1, 0]),
                  0x2000_0100: self.record("KHRV", [0, 0, 0, 0, (5 + 1) | (1 << 16), 1])}
        out = "\n".join(self.lines(memory))
        self.assertIn("check #0 (name hash", out)
        self.assertIn("no such name in this image", out)
        self.assertIn("check #5's onStarved was running, attempt 1", out)
        self.assertIn(
            "escalated to the policy after 1 recovery attempt(s)", out)

    def test_a_panic_in_the_guard_and_no_records_at_all(self):
        memory = {0x2000_0200: self.guard(3, 2, 2, 1, 2, 1)}
        self.assertIn("the last failure panic (stack smash)",
                      "\n".join(self.lines(memory)))
        self.assertEqual(kb.health_lines(
            [], lambda w, s: bytes(s), self.RESIDENT), [])

    @unittest.skipUnless(__import__("os").environ.get("KVASIR_HEALTH_ELF"),
                         "KVASIR_HEALTH_ELF=<an image with a health supervisor that has onStarved and names "
                         "'engine'> runs the decoder against a built image")
    def test_a_built_image(self):
        import os
        import subprocess
        elf = Path(os.environ["KVASIR_HEALTH_ELF"])
        sys.path.insert(0, str(HERE.parent / "cmake" / "tools"))
        import elf_image
        out = subprocess.run([shutil.which("llvm-nm") or "arm-none-eabi-nm", "-C", "-S", "--defined-only",
                              str(elf)], capture_output=True, text=True, check=True).stdout
        table = [(int(a, 16), int(n, 16), name) for a, n, name in
                 __import__("re").findall(r"^([0-9a-f]{8}) ([0-9a-f]{8}) [bBdD] (.+)$", out, __import__("re").M)]
        at = {name: addr for addr, _, name in table}
        memory = {at["Kvasir::Health::lastStarvation"]: self.record("KHLT", [2, kb.fnv1a(b"engine"), 1, 1, 1, 0]),
                  at["Kvasir::Health::lastRecovery"]: self.record("KHRV", [1, 2, kb.fnv1a(b"engine"), 5,
                                                                           3 | (1 << 16), 0])}
        if "Kvasir::BootGuard::state" in at:
            memory[at["Kvasir::BootGuard::state"]
                   ] = self.guard(4, 1, 1, 4, 2, 1)
        lines = kb.health_lines(table, lambda w, s: memory.get(w, bytes(s))[:s],
                                elf_image.Elf(elf.read_bytes()).resident())
        text = "\n".join(lines)
        self.assertIn("check 'engine' (#2) starved", text)
        self.assertIn("onStarved was running, attempt 1", text)
        if "Kvasir::BootGuard::state" in at:
            self.assertIn(
                "the last failure starvation (check 'engine' (#2))", text)


if __name__ == "__main__":
    unittest.main()
