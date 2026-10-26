#!/usr/bin/env python3
"""kvasir_bench's parts that need no board (tools/kvasir_bench.py and its package kvasir_bench_lib): the command
line's shape, the image and session seams, profile ranking, the record decoders, the printer guards, chip commands."""
import argparse
import atexit
import contextlib
import importlib.util
import io
import json
import os
import pathlib
import re
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import types
import unittest
import unittest.mock
from pathlib import Path

sys.dont_write_bytecode = True  # no __pycache__ next to the imported tools

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "tests" / "tooling"))
import armtc  # noqa: E402  when a missing tool is a skip or a failure

spec = importlib.util.spec_from_file_location(
    "kvasir_bench", HERE / "kvasir_bench.py")
assert spec is not None and spec.loader is not None
kb = importlib.util.module_from_spec(spec)
# puts tools/ and the package's tool directories on sys.path
spec.loader.exec_module(kb)
import uc_log_client  # noqa: E402
import kvasir_elf  # noqa: E402
from kvasir_bench_lib import (commands, coverage_data, gdb_stub, image, printer, profiling, records,  # noqa: E402
                              session, trees)


def fake_session(tree=None, control=None) -> session.Session:
    """A Session on a tree that need not exist, its printer's socket already found (`control`): no printer is
    looked for or started."""
    s = session.Session(
        tree if tree is not None else types.SimpleNamespace(build="b", target="t"))
    s._control = control
    return s


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
                    lines = [{"cmd": "subscribe"}, log,
                             {"cmd": "dropped", "lines": 7}]
                    conn.sendall(
                        "".join(json.dumps(x) + "\n" for x in lines).encode())
                    conn.recv(1)   # until the client closes

            thread = threading.Thread(target=printer, daemon=True)
            thread.start()
            # the timeout is no pass condition: far past ctest's, which is the hang guard
            args = types.SimpleNamespace(build="b", target="t", regex="never", timeout=1e6,
                                         from_now=True, from_start=False, module=None,
                                         not_module=None, width=200, last=5)
            out = io.StringIO()
            err = io.StringIO()
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err), \
                    self.assertRaises(SystemExit) as exit_:
                commands.wait_for(fake_session(control=path), args)
            self.assertEqual(exit_.exception.code, 5)
            self.assertIn("7 lines dropped for this client", out.getvalue())
            # a caller acts only after this line (scenario harnesses: no race with what it waits for)
            self.assertEqual(err.getvalue().splitlines()
                             [0], "wait-for: subscribed")
            server.close()


class WaitForHalt(unittest.TestCase):
    """A halt message (kind "halt") ends wait-for with exit 3, whatever its text says; a plain error whose text starts
    with "core halted" does not."""

    def test_the_kind_ends_the_wait_not_the_text(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "control.sock"
            server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            server.bind(str(path))
            server.listen(1)

            def printer():
                conn, _ = server.accept()
                with conn:
                    conn.recv(65536)   # the subscribe
                    lines = [{"cmd": "subscribe"},
                             {"cmd": "message", "time": "t", "level": "error",
                              "text": "core halted: a plain error that only says so"},
                             {"cmd": "message", "time": "t", "level": "error", "kind": "halt",
                              "text": "pc=0x10000124 (a halt whatever its text)"}]
                    conn.sendall(
                        "".join(json.dumps(x) + "\n" for x in lines).encode())
                    conn.recv(1)   # until the client closes

            thread = threading.Thread(target=printer, daemon=True)
            thread.start()
            args = types.SimpleNamespace(build="b", target="t", regex="never", timeout=1e6,
                                         from_now=True, from_start=False, module=None,
                                         not_module=None, width=200, last=5)
            out = io.StringIO()
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()), \
                    self.assertRaises(SystemExit) as exit_:
                commands.wait_for(fake_session(control=path), args)
            self.assertEqual(exit_.exception.code, 3)
            self.assertEqual(out.getvalue().splitlines()[0],
                             "CORE HALTED while waiting for /never/: pc=0x10000124 (a halt whatever its text)")
            server.close()


class MetricsDropped(unittest.TestCase):
    """A printer that dropped samples for this connection: metrics notes it and goes on (a rate is only short)."""

    def test_a_dropped_event_is_noted_and_the_stream_goes_on(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "control.sock"
            server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            server.bind(str(path))
            server.listen(1)

            def metric(value, time_):
                return {"cmd": "metric", "name": "a", "scope": "s", "unit": "mm", "time": time_, "value": value}

            def printer_side():
                conn, _ = server.accept()
                with conn:
                    conn.recv(65536)   # the subscribe
                    lines = [{"cmd": "subscribe"}, metric(1, 0.5),
                             {"cmd": "dropped", "lines": 3}, metric(2, 0.75)]
                    conn.sendall(
                        "".join(json.dumps(x) + "\n" for x in lines).encode())
                    conn.recv(1)   # until the client closes

            thread = threading.Thread(target=printer_side, daemon=True)
            thread.start()
            # the --until sample ends it; the seconds are no pass condition (ctest's timeout is the hang guard)
            args = types.SimpleNamespace(
                build="b", target="t", seconds=1e6, name=None, raw=True, until="a >= 2")
            out, err = io.StringIO(), io.StringIO()
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                commands.metrics(fake_session(control=path), args)
            server.close()
            self.assertEqual(out.getvalue().splitlines(), [
                f"{0.5:12.6f} s  {uc_log_client.metric_key(metric(1, 0.5))} = 1 mm",
                f"{0.75:12.6f} s  {uc_log_client.metric_key(metric(2, 0.75))} = 2 mm",
                f"{uc_log_client.metric_key(metric(2, 0.75))} = 2 mm at 0.750000 s (uc_time)"])
            self.assertIn("3 samples dropped for this client", err.getvalue())


class PrinterGdbStub(unittest.TestCase):
    """snapshot under a printer: gdb's remote protocol answered from the printer's registers and reads."""

    def exchange(self, packets: list[str]) -> list[str]:
        ours, theirs = socket.socketpair()
        memory = {0x20000000 + i: i & 0xFF for i in range(64)}
        regs = list(range(0x100, 0x111))
        thread = threading.Thread(target=gdb_stub.serve_gdb, args=(
            theirs, regs,
            lambda a, n: bytes(memory[a + i] for i in range(n)) if all(a + i in memory for i in range(n))
            else None))
        thread.start()
        replies = []
        with ours:
            for p in packets:
                ours.sendall(gdb_stub.gdb_packet(p))
                data = b""
                while not (data.count(b"#") and len(data) >= data.rindex(b"#") + 3):
                    data += ours.recv(65536)
                body = data[data.index(b"$") + 1:data.rindex(b"#")].decode()
                self.assertEqual(data[:1], b"+", "every packet acknowledged")
                replies.append(body)
        thread.join()
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


def load_in_gdb():
    """in_gdb.py as gdb loads it: a top-level module, not a package member."""
    path = HERE / "kvasir_bench_lib" / "in_gdb.py"
    spec_ = importlib.util.spec_from_file_location("in_gdb", path)
    assert spec_ is not None and spec_.loader is not None
    module = importlib.util.module_from_spec(spec_)
    spec_.loader.exec_module(module)
    return module, path


class GdbSide(unittest.TestCase):
    """The snapshot's print formats, now plain functions in in_gdb.py: the text of the %-format strings the gdb
    script had before (copied here as the oracle), apart from the name form (cxx_names)."""

    def setUp(self):
        self.in_gdb, self.path = load_in_gdb()

    def test_mode_like_the_old_script(self):
        def old(xpsr):
            xpsr &= 0x1ff
            return "thread" if xpsr == 0 else "exception %d (%s)" % (
                xpsr, {3: "HardFault", 15: "SysTick"}.get(xpsr, "IRQ %d" % (xpsr - 16)))
        for xpsr in (0x61000000, 0x61000003, 0x0100000F, 0x01000010, 0x0100002A, 0x1FF):
            self.assertEqual(self.in_gdb.mode_text(xpsr), old(xpsr), hex(xpsr))

    def test_frame_and_variable_lines_like_the_old_script(self):
        root = "/src/work/"
        self.assertEqual(self.in_gdb.frame_line(3, "main", "/src/work/fw/src/main.cpp", 42, root),
                         "#%-2d %s  %s:%s" % (3, "main", "fw/src/main.cpp", 42))
        self.assertEqual(self.in_gdb.frame_line(
            12, None, "?", 0, ""), "#12 ?  ?:0")
        self.assertEqual(self.in_gdb.frame_line(0, "a::B<int>::f(int)", "/x/a.hpp", 7, root),
                         "#0  a::B<>::f()  /x/a.hpp:7")
        for name, size, raw in (("Kvasir::x", 4, (0x1234).to_bytes(4, "little")), ("y" * 80, 2, b"\x01\x00"),
                                ("buffer", 16, bytes(range(16)))):
            value = int.from_bytes(raw, "little")
            self.assertEqual(self.in_gdb.var_line(name, size, raw),
                             "%s = %s" % (name[:70], "%d (0x%x)" % (value, value) if size <= 8 else raw.hex(" ")))

    def test_no_relative_import(self):
        self.assertNotRegex(self.path.read_text(
        ), r"(?m)^\s*from \.", "inside gdb it is no package member")

    def test_the_gdb_command_line_imports_it_without_bytecode(self):
        ex = gdb_stub.gdb_python("snapshot", Path("/tmp/x/job.json"))
        self.assertEqual(ex[0::2], ["-ex", "-ex"])
        self.assertIn("sys.dont_write_bytecode = True", ex[1])
        self.assertIn(str(HERE), ex[1], "tools/, for cxx_names")
        self.assertEqual(
            ex[3], "python import in_gdb; in_gdb.snapshot('/tmp/x/job.json')")

    @unittest.skipUnless(os.environ.get("KVASIR_GDB_ELF"),
                         "KVASIR_GDB_ELF=<an ELF> runs peek --typed's decoder in a real gdb (no target, no probe)")
    def test_typed_in_a_real_gdb(self):
        elf = Path(os.environ["KVASIR_GDB_ELF"])
        if gdb_stub.arm_gdb() is None:
            self.skipTest("no gdb with Python and ARM support")
        symbol = next(s for s in image.Image(elf).objects if 4 <= s.size <= 64)
        text, = gdb_stub.decode_typed(types.SimpleNamespace(
            elf=elf), [(symbol.address, bytes(symbol.size))])
        self.assertIsNotNone(text, symbol.name)


class Profile(unittest.TestCase):
    def test_mangled_names_behind_prefixes(self):
        armtc.first(self, "llvm-cxxfilt", "c++filt")   # the demangler
        short = profiling.short_function_names(
            {"", "main", "_ZN6Kvasir4Test3runILi3EEEvi", "__Thumbv7ABSLongThunk__ZN6Kvasir4Test3runEv",
             ".Lswitch.table._ZNK6Kvasir3Foo3barEv"})
        self.assertEqual(short[""], "??")
        self.assertEqual(short["main"], "main")
        self.assertEqual(
            short["_ZN6Kvasir4Test3runILi3EEEvi"], "void Kvasir::Test::run<>()")
        self.assertEqual(short["__Thumbv7ABSLongThunk__ZN6Kvasir4Test3runEv"],
                         "__Thumbv7ABSLongThunk_Kvasir::Test::run()")
        self.assertEqual(short[".Lswitch.table._ZNK6Kvasir3Foo3barEv"],
                         ".Lswitch.table.Kvasir::Foo::bar() const")

    def test_same_file_name_in_two_folders(self):
        frames = {0x100: [{"FunctionName": "f()", "FileName": "/s/I2C/Device.hpp", "Line": 3}],
                  0x200: [{"FunctionName": "g()", "FileName": "/s/SPI/Device.hpp", "Line": 3}]}
        ranked = profiling.rank_samples({0x100: 1, 0x200: 1}, frames, top=5)
        self.assertEqual(sorted(name for _, name in ranked["line"]), [
                         "I2C/Device.hpp:3", "SPI/Device.hpp:3"])

    def test_ranking(self):
        frames = {
            0x100: [{"FunctionName": "leaf<int>()", "FileName": "/x/Engine.hpp", "Line": 10},
                    {"FunctionName": "outer()", "FileName": "/x/Device.hpp", "Line": 5}],
            0x200: [{"FunctionName": "outer()", "FileName": "/x/Device.hpp", "Line": 7}],
        }
        ranked = profiling.rank_samples(
            {0x100: 3, 0x200: 1, 0x300: 4}, frames, top=5)
        self.assertEqual(ranked["self"], [(50.0, "??"),
                         (37.5, "leaf<>()"), (12.5, "outer()")])
        # once per function on the inline stack
        self.assertIn((50.0, "outer()"), ranked["inclusive"])
        self.assertIn((37.5, "Engine.hpp:10"), ranked["line"])

    NM = ("10000100 00000004 T std::optional<unsigned short>::has_value() const\n"
          "10000100 00000004 t FPBits<float>::is_neg() const\n"
          "10000100 00000004 t std::optional<bool>::has_value() const\n"
          "10000200 00000010 T outer()\n"
          "20000000 00000004 B a_variable\n")

    def table(self):
        return image.function_table(self.NM)

    def test_folded_bodies_are_one_symbol_with_every_name(self):
        table = self.table()
        self.assertEqual([start for start, _, _ in table],
                         [0x10000100, 0x10000200])
        self.assertEqual(len(table[0][2]), 3)

    def test_an_address_belongs_to_the_body_it_lies_in(self):
        table = self.table()
        self.assertEqual(profiling.symbol_at(table, 0x10000102)[0], 0x10000100)
        self.assertEqual(profiling.symbol_at(table, 0x1000020e)[0], 0x10000200)
        self.assertIsNone(profiling.symbol_at(
            table, 0x10000104))   # between the two
        self.assertIsNone(profiling.symbol_at(
            table, 0x20000000))   # data, not code

    def test_ranking_by_symbol_names_the_folded_body(self):
        ranked = profiling.rank_by_symbol(
            {0x10000100: 3, 0x10000102: 3, 0x10000204: 2}, self.table(), top=5)
        self.assertEqual(ranked[1], (25.0, "outer()"))
        self.assertAlmostEqual(ranked[0][0], 75.0)
        self.assertIn("[3 folded]", ranked[0][1])
        self.assertIn("is_neg", ranked[0][1])

    def test_a_zero_size_label_is_not_a_folded_function(self):
        nm_out = ("10000130 00000000 T _LINKER_INTERN_core1_vectors_end_\n"
                  "10000130 00012018 t main_body\n"
                  "10000400 00000000 T asm_stub\n")
        table = image.function_table(nm_out)
        self.assertEqual(
            table, [(0x10000130, 0x12018, ["main_body"]), (0x10000400, 0, ["asm_stub"])])
        self.assertEqual(profiling.symbol_name(
            profiling.symbol_at(table, 0x10000140)[1], {}), "main_body")
        self.assertEqual(profiling.symbol_at(
            table, 0x10000401)[1], ["asm_stub"])

    def test_the_table_is_read_mangled_and_only_hit_bodies_are_demangled(self):
        asked = []
        nm_out = ("10000100 00000004 T _ZNKSt8optionalItE9has_valueEv\n"
                  "10000100 00000004 t _ZNK6FPBitsIfE6is_negEv\n"
                  "10000200 00000010 T _Z5outerv\n"
                  "10000300 00000010 T _Z9never_hitv\n")
        table = image.Image(pathlib.Path("t.elf"),
                            run_nm=lambda elf, demangle: asked.append(demangle) or nm_out).functions
        self.assertEqual(asked, [False])
        short = profiling.symbol_names(table, [0x10000102, 0x10000204])
        self.assertNotIn("_Z9never_hitv", short)
        self.assertEqual(short["_Z5outerv"], "outer()")
        ranked = profiling.rank_by_symbol(
            {0x10000102: 1, 0x10000204: 3}, table, top=5, short=short)
        self.assertEqual(ranked[0], (75.0, "outer()"))
        self.assertIn("FPBits<>::is_neg()", ranked[1][1])
        self.assertIn("[2 folded]", ranked[1][1])


# uc_log's fixture target (tests/data/records) through the printer's `records` / `raise_panic` requests: the answers
# its control-protocol golden holds (doc/control_protocol/records_fixture.jsonl) - the texts below are written from them
FIXTURE = HERE.parent / "uc_log" / "tests" / "data" / "records"
RECORDS_FIXTURE = {x["name"]: x for x in map(json.loads, (HERE.parent / "uc_log" / "doc" / "control_protocol" /
                                                          "records_fixture.jsonl").read_text().splitlines())}


def fixture_answer(name: str) -> dict:
    """The printer's answer to the fixture case `name`."""
    return RECORDS_FIXTURE[name]["answer"]


# full_records' text before the record table (printed by that code on the records below)
GOLDEN_1 = [
    '  the fault record (the first of 2 since a boot line last reported one), core 2, Armv8-M, frameValid:',
    '    cfsr 0x00008200 (PRECISERR BFARVALID) hfsr 0x00000000 mmfar 0x00000000 bfar 0x40000000 icsr 0x00000000',
    '    faulting pc 0x10000124  f_10000124()  x.cpp:4',
    '    its lr      0x10000201  f_10000200()  x.cpp:0',
    '    r0=0x1 r1=0x0 r2=0x0 r3=0x0 r12=0x0 xpsr=0x0 r4=0x0 r5=0x0 r6=0x0 r7=0x0 r8=0x0 r9=0x0 r10=0x0 r11=0x0 exc_return=0x0 msp=0x0 psp=0x0 control=0x0 sp=0x20001f00',
    '    shcsr=0x0 icsr=0x0 sfsr=0x0 sfar=0x0 msplim=0x20000000 psplim=0x0',
    '  the stack snapshot: 16 bytes from 0x20001f00; the words that point into code:',
    '    0x20001f00 0x10000201  f_10000200()  x.cpp:0',
    '    0x20001f08 0x10000125  f_10000124()  x.cpp:4',
    '  the panic record (the first of 1 since a boot line last reported one): unhandled interrupt, exception 42 = IRQ 26, core 1, sp 0x20001e00, at 4294967301 us, raised by the call before 0x10000301  f_100002fe()  x.cpp:14',
]
GOLDEN_2 = [
    '  the fault record (the first of 2 since a boot line last reported one), core 0, Armv0-M, calleeSaved:',
    '    cfsr 0x00008200 (PRECISERR BFARVALID) hfsr 0x00000000 mmfar 0x00000000 bfar 0x40000000 icsr 0x00000000',
    '    no stacked frame (it was not pushed, or lies outside RAM)',
    '    r0=0x1 r1=0x0 r2=0x0 r3=0x0 r12=0x0 xpsr=0x0 r4=0x0 r5=0x0 r6=0x0 r7=0x0 r8=0x0 r9=0x0 r10=0x0 r11=0x0 exc_return=0x0 msp=0x0 psp=0x0 control=0x0 sp=0x20001f00',
    '    shcsr=0x0 icsr=0x0 sfsr=0x0 sfar=0x0 msplim=0x20000000 psplim=0x0',
    '  the stack snapshot: 16 bytes from 0x20001f00; the words that point into code:',
    '    0x20001f00 0x10000201  f_10000200()  x.cpp:0',
    '    0x20001f08 0x10000125  f_10000124()  x.cpp:4',
    '  the panic record (the first of 1 since a boot line last reported one): check failed, detail 3, core 1, sp 0x20001e00, at 1234 us, raised by the call before 0x10000301  f_100002fe()  x.cpp:14',
]
GOLDEN_3 = [
    '  the fault record (lastFault) is empty or not valid: no fault since a boot line reported one',
    '  the panic record (the first of 1 since a boot line last reported one): abort, core 1, sp 0x20001e00, time 0 (no counter, or it did not run), no program site',
]
GOLDEN_4 = [
    '  the fault record (lastFault) is empty or not valid: no fault since a boot line reported one',
    '  the panic record (the first of 1 since a boot line last reported one): cause 99, core 1, sp 0x20001e00, at 1234 us, raised by the call before 0x10000301  f_100002fe()  x.cpp:14',
]
GOLDEN_5 = [
    '  the fault record (lastFault) is empty or not valid: no fault since a boot line reported one',
    '  the panic record (lastPanic) is empty or not valid: no panic since a boot line reported one',
]


class FullRecordsText(unittest.TestCase):
    """`crash`'s text for the crash records, pinned before the printer became their one decoder: the same records,
    now in uc_log's fixture target (crash_golden_N), give the same text from the printer's answer."""

    def lines(self, case):
        s = types.SimpleNamespace(tree=None, image=types.SimpleNamespace(
            is_code=lambda a: 0x10000000 <= a < 0x10010000))
        out = io.StringIO()
        with unittest.mock.patch.object(records, "addr2line",
                                        lambda tree, addrs: {a: f"f_{a:x}()  x.cpp:{a & 0xf}" for a in addrs}), \
                contextlib.redirect_stdout(out):
            records.full_records(s, fixture_answer(case))
        return out.getvalue().splitlines()

    def test_a_fault_with_its_frame_the_snapshot_and_an_unhandled_interrupt(self):
        self.assertEqual(self.lines("crash_golden_1"), GOLDEN_1)

    def test_a_fault_without_a_frame_and_a_panic_with_a_detail(self):
        self.assertEqual(self.lines("crash_golden_2"), GOLDEN_2)

    def test_a_panic_without_a_program_site_and_no_fault(self):
        self.assertEqual(self.lines("crash_golden_3"), GOLDEN_3)

    def test_empty_records_and_a_cause_past_the_table(self):
        self.assertEqual(self.lines("crash_golden_4"), GOLDEN_4)
        self.assertEqual(self.lines("records_crash_health_empty"), GOLDEN_5)
        self.assertEqual(self.lines("records_crash_health_no_symbols"), [
        ], "an image without the records says nothing")


class PanicCommand(unittest.TestCase):
    """`panic` asks the printer to make the core call Kvasir::Panic::raise(cause): its raise_panic request, the
    cause as typed (the printer knows the names and lists them in its error)."""

    def run_panic(self, cause):
        sent = []
        s = fake_session(control=pathlib.Path("sock"))
        s.ask = lambda req, timeout=None: sent.append(
            req) or {"cmd": "raise_panic"}
        with unittest.mock.patch.object(printer, "set_mark"), contextlib.redirect_stdout(io.StringIO()):
            commands.panic(s, types.SimpleNamespace(cause=cause))
        return sent

    def test_the_request(self):
        self.assertEqual(self.run_panic("check-failed"),
                         [{"cmd": "raise_panic", "cause": "check-failed"}])
        self.assertEqual(self.run_panic("stack overflow"), [
                         {"cmd": "raise_panic", "cause": "stack overflow"}])

    def test_an_unknown_cause_is_the_printer_s_error(self):
        error = fixture_answer("raise_panic_unknown_cause")["error"]
        self.assertIn("check-failed", error)
        self.assertIn("user", error)


class MapCheck(unittest.TestCase):
    """A records answer is decoded with the printer's map: it must be this tree's (another target's printer would
    decode any address as plausibly), and no --stale-ok gets past that."""

    def session(self, build, path):
        tree = trees.Tree.__new__(trees.Tree)
        tree.build, tree.target, tree.cache = Path(build), "t", ""
        Path(build, "t.printer_args").write_text(
            "--device\nD\n--map_file\nt.map\n")
        s = fake_session(tree, control=pathlib.Path("sock"))
        s.ask = lambda req, timeout=None: {
            "cmd": "records", "map": {"path": str(path), "generation": 3}}
        return s

    def test_the_tree_s_map_passes(self):
        with tempfile.TemporaryDirectory() as b:
            self.assertEqual(self.session(
                b, Path(b) / "t.map").records(["crash"])["map"]["generation"], 3)

    def test_another_map_dies(self):
        with tempfile.TemporaryDirectory() as b, contextlib.redirect_stderr(io.StringIO()) as err:
            with self.assertRaises(SystemExit) as e:
                self.session(b, Path(b) / "other.map").records(["crash"])
        self.assertEqual(e.exception.code, 1)
        self.assertIn("not this tree's map", err.getvalue())


class ResetLine(unittest.TestCase):
    """`reset --line` asks the printer for a reset through the probe's reset pin ("line": true)."""

    def test_request(self):
        self.assertEqual(uc_log_client.req_reset(), {"cmd": "reset"})
        self.assertEqual(uc_log_client.req_reset(line=True),
                         {"cmd": "reset", "line": True})

    def test_reset_sends_line(self):
        sent = []
        args = types.SimpleNamespace(
            build="b", target="t", confirm=False, line=True)
        with unittest.mock.patch.object(printer, "need_ram_image_printer"), \
                unittest.mock.patch.object(printer, "set_mark"), \
                unittest.mock.patch.object(printer, "firmware_line", return_value=""), \
                unittest.mock.patch.object(commands, "request", side_effect=lambda c, r, **_: sent.append(r) or {}), \
                contextlib.redirect_stdout(io.StringIO()):
            commands.reset(fake_session(control="sock"), args)
        self.assertEqual(sent, [{"cmd": "reset", "line": True}])


class TreeSettings(unittest.TestCase):
    """The target's printer options come from the tree's <target>.printer_args, one argument a line."""

    def tree(self, build, cache="", args=("--device", "D")):
        tree = trees.Tree.__new__(trees.Tree)
        tree.build, tree.target, tree.cache = Path(build), "t", cache
        if args is not None:
            Path(build, "t.printer_args").write_text(
                "".join(f"{a}\n" for a in args))
        return tree

    def test_the_environment_overrides_the_file_even_when_empty(self):
        with tempfile.TemporaryDirectory() as b:
            tree = self.tree(b, args=("--device", "D", "--host", "192.0.2.10"))
            with unittest.mock.patch.dict(os.environ, {"JLINK_IP": ""}):
                self.assertEqual(tree._setting("JLINK_IP"), "")
            with unittest.mock.patch.dict(os.environ, {}, clear=True):
                self.assertEqual(tree._setting("JLINK_IP"), "192.0.2.10")
                self.assertEqual(tree._setting("JLINK_PROBE"),
                                 "", "options without --probe name no USB probe")

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
            with unittest.mock.patch.dict(os.environ, {}, clear=True):
                self.assertEqual(tree._setting("JLINK_PROBE"), "Base Probe")

    def test_the_printer_command_is_the_file_with_the_bench_s_own_probe_and_log(self):
        with tempfile.TemporaryDirectory() as b:
            tree = self.tree(b, args=("--device", "D", "--speed", "4000", "--probe", "Old",
                                      "--pre_reset_command", "w4 0x5 0x6", "--ram_image"))
            tree.log_dir = Path(b, "log")
            tree.host, tree.probe = "", "New"
            with unittest.mock.patch.dict(os.environ, {}, clear=True):
                cmd = tree.printer_command(Path("/p"))
            self.assertEqual(cmd, ["/p", "--device", "D", "--speed", "4000", "--pre_reset_command", "w4 0x5 0x6",
                                   "--ram_image", "--probe", "New", "--build_command", "true",
                                   "--log_dir", str(Path(b, "log")), "--disable_ui"])
            tree.host = "192.0.2.30"
            with unittest.mock.patch.dict(os.environ, {"JLINK_DEVICE": "E"}, clear=True):
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
                return subprocess.CompletedProcess(cmd, 0, "", "")

            started = []

            class Proc:
                pid = 7

                def __init__(self, cmd, **_kw):
                    started.append(cmd)

                def poll(self):
                    return None

            with unittest.mock.patch.dict(os.environ, {"UC_LOG_PRINTER_BIN": sys.executable}), \
                    unittest.mock.patch.object(subprocess, "run", run), \
                    unittest.mock.patch.object(subprocess, "Popen", Proc), \
                    unittest.mock.patch.object(printer, "control_socket", return_value=None), \
                    unittest.mock.patch.object(printer, "printers", return_value=[]), \
                    unittest.mock.patch.object(time, "sleep"):
                self.assertEqual(printer.start_printer(tree), 7)
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
            with unittest.mock.patch.object(printer, "printers", return_value=[printer_row(4242, ["p", "--disable_ui", "--log_dir", str(tree.log_dir)])]), \
                    unittest.mock.patch.object(os, "kill") as kill, \
                    unittest.mock.patch.object(pathlib.Path, "exists", lambda self: exists(self)), \
                    unittest.mock.patch.object(time, "sleep"), \
                    unittest.mock.patch.object(time, "monotonic", lambda: next(clock) * 0.05), \
                    contextlib.redirect_stdout(io.StringIO()) as out, contextlib.redirect_stderr(io.StringIO()) as err:
                try:
                    commands.printer_stop(fake_session(tree), args)
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
            with unittest.mock.patch.object(printer, "control_socket", lambda t: next(answers)), \
                    unittest.mock.patch.object(printer, "printers", return_value=[printer_row(os.getpid(), ["p", "--log_dir", str(tree.log_dir)])]), \
                    unittest.mock.patch.object(printer, "start_printer") as start, \
                    unittest.mock.patch.object(time, "sleep"):
                self.assertEqual(printer.need_printer(
                    tree, "t"), Path(b, "sock"))
                start.assert_not_called()

    def test_one_that_never_answers_is_refused_not_doubled(self):
        with tempfile.TemporaryDirectory() as b:
            tree = self.tree(b)
            clock = iter(range(0, 100))
            with unittest.mock.patch.object(printer, "control_socket", return_value=None), \
                    unittest.mock.patch.object(printer, "printers", return_value=[printer_row(os.getpid(), ["p", "--log_dir", str(tree.log_dir)])]), \
                    unittest.mock.patch.object(printer, "start_printer") as start, \
                    unittest.mock.patch.object(time, "sleep"), \
                    unittest.mock.patch.object(time, "monotonic", lambda: next(clock)), \
                    contextlib.redirect_stderr(io.StringIO()) as err, self.assertRaises(SystemExit) as e:
                printer.need_printer(tree, "t")
            start.assert_not_called()
            self.assertEqual(e.exception.code, 4)
            self.assertIn("not starting a second one", err.getvalue())


def printer_row(pid: int, argv: list[str], cwd: Path | None = None):
    """A row of printer.printers() for a printer started with argv."""
    return printer.Printer(pid, argv, 1.0, cwd)


class PrinterOfTarget(unittest.TestCase):
    """A target's printers are the ones on its own log directory: `x_release` is not served by `x_release_log`'s
    printer, although the one log directory is a prefix of the other."""

    def trees(self, b):
        def tree(target):
            t = TreeSettings.tree(None, b)
            t.target = target
            t.log_dir = Path(b, "rtt_log", target).resolve()
            return t
        return tree("x_release"), tree("x_release_log")

    def stop(self, b, tree, rows):
        args = types.SimpleNamespace(
            build=b, target=tree.target, confirm=False)
        with unittest.mock.patch.object(printer, "printers", return_value=rows), \
                unittest.mock.patch.object(os, "kill") as kill, \
                unittest.mock.patch.object(time, "sleep"), \
                contextlib.redirect_stdout(io.StringIO()) as out, contextlib.redirect_stderr(io.StringIO()) as err:
            try:
                commands.printer_stop(fake_session(tree), args)
                code = 0
            except SystemExit as e:
                code = e.code
        return code, out.getvalue() + err.getvalue(), kill

    def test_stop_leaves_a_headless_printer_of_the_longer_name(self):
        with tempfile.TemporaryDirectory() as b:
            mine, other = self.trees(b)
            code, text, kill = self.stop(b, mine, [printer_row(
                4242, ["p", "--disable_ui", "--log_dir", str(other.log_dir)])])
            kill.assert_not_called()
            self.assertEqual(code, 0)
            self.assertIn("no printer of this target is running", text)

    def test_stop_is_not_refused_by_a_window_of_the_longer_name(self):
        with tempfile.TemporaryDirectory() as b:
            mine, other = self.trees(b)
            code, text, kill = self.stop(b, mine, [printer_row(
                4242, ["p", "--log_dir", str(other.log_dir)])])
            kill.assert_not_called()
            self.assertEqual(code, 0, text)

    def test_need_printer_does_not_wait_for_the_longer_name(self):
        with tempfile.TemporaryDirectory() as b:
            mine, other = self.trees(b)
            clock = iter(range(0, 100))
            started = []

            def start_printer(tree):
                started.append(tree)
                return os.getpid()
            # only the printer started for x_release answers on x_release's socket
            with unittest.mock.patch.object(printer, "control_socket", lambda t: Path(b, "sock") if started else None), \
                    unittest.mock.patch.object(printer, "printers", return_value=[printer_row(
                        os.getpid(), ["p", "--disable_ui", "--log_dir", str(other.log_dir)])]), \
                    unittest.mock.patch.object(printer, "start_printer", side_effect=start_printer) as start, \
                    unittest.mock.patch.object(printer, "_started_here", []), \
                    unittest.mock.patch.object(atexit, "register"), \
                    unittest.mock.patch.object(time, "sleep"), \
                    unittest.mock.patch.object(time, "monotonic", lambda: next(clock)), \
                    contextlib.redirect_stderr(io.StringIO()) as err:
                try:
                    control = printer.need_printer(mine, "t")
                except SystemExit as e:
                    self.fail(f"refused (exit {e.code}): {err.getvalue()}")
            start.assert_called_once_with(mine)
            self.assertEqual(control, Path(b, "sock"))

    def test_stop_finds_its_own_printer_through_a_symlink_and_a_relative_log_dir(self):
        with tempfile.TemporaryDirectory() as b:
            _, other = self.trees(b)
            Path(b, "link").symlink_to(b)
            rows = [printer_row(4242, ["p", "--disable_ui", "--log_dir", str(Path(b, "link", "rtt_log", other.target))]),
                    printer_row(4343, ["p", "--disable_ui", f"--log_dir=rtt_log/{other.target}"], Path(b, "link"))]
            with unittest.mock.patch.object(printer.Printer, "alive", lambda p: False):
                code, text, kill = self.stop(b, other, rows)
            self.assertEqual(code, 0, text)
            self.assertEqual(
                sorted(c.args[0] for c in kill.call_args_list), [4242, 4343])


class PrinterRecord(unittest.TestCase):
    """printer.Printer reads a printer's options from its argv, never from a space-joined command line."""

    def test_a_probe_nickname_with_spaces_stays_whole(self):
        p = printer_row(1, ["/x/uc_log_printer", "--probe",
                        "Probe One", "--log_dir", "/d"])
        self.assertEqual(p.probe, "Probe One")
        self.assertEqual(printer_row(
            1, ["/x/uc_log_printer", "--probe", "A --B", "--disable_ui"]).probe, "A --B")

    def test_host_any_usb_and_window(self):
        p = printer_row(1, ["/x/uc_log_printer", "--host",
                        "192.0.2.10", "--disable_ui"])
        self.assertEqual(p.probe, "ip:192.0.2.10")
        self.assertFalse(p.has_window)
        q = printer_row(1, ["/x/uc_log_printer"])
        self.assertEqual(q.probe, "(any usb)")
        self.assertTrue(q.has_window)
        self.assertIsNone(q.log_dir)

    def test_log_dir_forms(self):
        with tempfile.TemporaryDirectory() as b:
            b = Path(b).resolve()
            Path(b, "link").symlink_to(b)
            self.assertEqual(printer_row(
                1, ["p", "--log_dir=/a/b"]).log_dir, Path("/a/b"))
            self.assertEqual(printer_row(
                1, ["p", "--log_dir", "rtt_log/t"], b).log_dir, b / "rtt_log" / "t")
            self.assertEqual(printer_row(1, ["p", "--log_dir", str(b / "link" / "rtt_log" / "t")]).log_dir,
                             b / "rtt_log" / "t")

    def test_from_proc(self):
        with tempfile.TemporaryDirectory() as b:
            proc = Path(b, "123")
            proc.mkdir()
            (proc / "cmdline").write_bytes(b"/x/build/uc_log_printer\0--probe\0Probe One\0--log_dir\0rtt_log/t\0")
            (proc / "cwd").symlink_to(b)
            p = printer.Printer.from_proc(proc)
            self.assertEqual(p.pid, 123)
            self.assertEqual(p.argv, [
                             "/x/build/uc_log_printer", "--probe", "Probe One", "--log_dir", "rtt_log/t"])
            self.assertEqual(p.probe, "Probe One")
            self.assertEqual(p.log_dir, Path(b).resolve() / "rtt_log" / "t")
            (proc / "cmdline").write_bytes(b"/usr/bin/python3\0uc_log_printer\0")
            self.assertIsNone(printer.Printer.from_proc(proc))


class ShortPath(unittest.TestCase):
    def tree(self, cache):
        with tempfile.TemporaryDirectory() as b:
            Path(b, "CMakeCache.txt").write_text(cache)
            Path(b, "t.elf").touch()
            Path(b, "t.printer_args").write_text("--device\nD\n")
            return trees.Tree(b, "t")

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
        tree = type("Tree", (), {
                    "ram_image": lambda self: ram_image, "build": "b", "target": "t"})()
        err = io.StringIO()
        with unittest.mock.patch.object(printer, "request", lambda control, req: status), \
                contextlib.redirect_stderr(err):
            try:
                printer.need_ram_image_printer(
                    tree, Path("control.sock"), "flash")
            except SystemExit as e:
                return e.code, err.getvalue()
        return 0, err.getvalue()

    def test_a_printer_that_starts_ram_images_is_used(self):
        self.assertEqual(self.guard({"ram_image": True}), (0, ""))

    def test_an_ordinary_image_needs_nothing(self):
        self.assertEqual(self.guard({}, ram_image=False), (0, ""))

    def test_a_printer_started_without_the_flag_is_refused(self):
        code, text = self.guard({"ram_image": False})
        self.assertEqual(code, 4)
        self.assertIn("started without --ram_image", text)


class ChipPlugin(unittest.TestCase):
    def tree(self, chip_root):
        return type("Tree", (), {"chip_root": chip_root})()

    def listing(self, chip_root):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            commands.chip_cmd(fake_session(self.tree(chip_root)),
                              type("Args", (), {"build": "b", "target": "t", "rest": []})())
        return out.getvalue()

    def test_a_chips_commands_are_listed(self):
        with tempfile.TemporaryDirectory() as root:
            Path(root, "tools").mkdir()
            Path(root, "tools", "kvasir_bench_chip.py").write_text(
                "def commands():\n    return {'xyz': ('does xyz', lambda p: None, lambda b, a: None)}\n")
            self.assertIsNotNone(commands.load_chip_plugin(self.tree(root)))
            self.assertIn("xyz        does xyz", self.listing(root))

    def test_no_file_no_commands(self):
        with tempfile.TemporaryDirectory() as root:
            self.assertIsNone(commands.load_chip_plugin(self.tree(root)))
            self.assertIn("no chip commands", self.listing(root))
        self.assertIsNone(commands.load_chip_plugin(self.tree("")))


class CoverageManifest(unittest.TestCase):
    """assemble_profraw: Kvasir::Coverage's manifest (12-byte pieces on the target) into the .profraw bytes."""

    @staticmethod
    def manifest(pieces, header, magic=coverage_data.COVERAGE_MAGIC, status=0):
        raw = bytearray(struct.pack(
            "<4I", magic, status, len(pieces), len(header)))
        for p in pieces:
            raw += struct.pack("<3I", *p)
        raw += bytes(16 + 12 * coverage_data.COVERAGE_PIECES - len(raw))
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

        out = coverage_data.assemble_profraw(
            raw, lambda a, n: flash.get(a, None) and flash[a][:n], live)
        self.assertEqual(out, b"HEAD" + b"RECORDS!" +
                         b"\0\0" + b"\x00\xff\x00" + b"names" + b"xy")
        # only the counters come from the board
        self.assertEqual(reads, [0x20000100])

    def test_refuses_an_empty_or_failed_manifest(self):
        with self.assertRaisesRegex(ValueError, "not run Kvasir::Coverage"):
            coverage_data.assemble_profraw(self.manifest(
                [], b"", magic=0), lambda a, n: None, lambda a, n: b"")
        with self.assertRaisesRegex(ValueError, "reported 1"):
            coverage_data.assemble_profraw(self.manifest(
                [], b"", status=1), lambda a, n: None, lambda a, n: b"")


def tiny_elf(sections):
    """A little-endian ELF32 image: `sections` are (name, sh_flags, address, load address, bytes), each in a PT_LOAD
    segment of its own, followed by .shstrtab."""
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
        return coverage_data.image_reader(kvasir_elf.Elf(tiny_elf(sections)).resident())

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
        files = [HERE / "kvasir_bench.py", *
                 sorted((HERE / "kvasir_bench_lib").glob("*.py"))]
        self.assertGreater(
            len(files), 10, "the entry and every module of the package")
        for path in files:
            code = "\n".join(l for l in path.read_text(
            ).splitlines() if not l.lstrip().startswith("#"))
            for forbidden in ("JLinkExe", "JLinkGDBServer", "JLinkRTT", "libjlinkarm", "pylink",
                              "ctypes", "-CommandFile"):
                self.assertNotIn(
                    forbidden, code, f"{path.name} names {forbidden}")

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
            self.assertEqual(printer.probes_on_usb(Path(d)),
                             [("12345678", "J-Link Bench One", "Bench One"), ("87654321", "J-Link", "")])


class HealthRecords(unittest.TestCase):
    """crash's view of Health's KHLT/KHRV and the boot guard's KBTG records, from the printer's answers (uc_log's
    fixture target, health_* cases); the check names from the image's strings."""
    RESIDENT = [
        (".text", 0x1000_0000, b"\x00\x01tick\x00engine\x00core1\x00\xffxled\x00")]

    def lines(self, case):
        return records.health_lines(fixture_answer(case), self.RESIDENT)

    def test_fnv1a_is_the_header_s(self):
        # the FNV-1a reference values; HealthRecords.hpp's loop is the same offset basis and prime
        self.assertEqual(records.fnv1a(b""), 0x811C9DC5)
        self.assertEqual(records.fnv1a(b"a"), 0xE40C292C)
        self.assertEqual(records.fnv1a(b"foobar"), 0xBF9CF968)
        header = (HERE.parent / "src/kvasir/Health/HealthRecords.hpp").read_text()
        self.assertIn("0x811C'9DC5U", header)
        self.assertIn("0x0100'0193U", header)

    def test_names_whole_strings_and_merged_tails(self):
        wanted = {records.fnv1a(b"engine"), records.fnv1a(
            b"led"), records.fnv1a(b"nowhere")}
        self.assertEqual(records.names_by_hash(self.RESIDENT, wanted),
                         {records.fnv1a(b"engine"): "engine", records.fnv1a(b"led"): "led"})

    def test_a_starvation(self):
        out = self.lines("health_a_starvation")
        self.assertIn(
            "check 'engine' (#1) starved, silent 812 ms of the 500 ms allowed, 4321 ms after boot", out[0])
        self.assertIn("0x6", out[0])
        self.assertIn("lastRecovery) is empty", out[1])

    def test_a_reset_inside_on_starved_names_the_check_and_the_attempt(self):
        out = "\n".join(self.lines("health_a_reset_inside_on_starved"))
        self.assertIn(
            "2 recoveries this run, the last check 'core1' (#3) at 9000 ms", out)
        self.assertIn(
            "check 'core1' (#3)'s onStarved was running, attempt 2", out)
        self.assertIn("boot 7, 1 failed run(s) in a row, 4 in all, the last failure starvation "
                      "(check 'core1' (#3)); runOpen healthyThisRun", out)

    def test_an_index_without_a_hash_and_a_hash_not_in_the_image(self):
        out = "\n".join(self.lines("health_an_index_without_a_hash"))
        self.assertIn("check #0 (name hash", out)
        self.assertIn("no such name in this image", out)
        self.assertIn("check #5's onStarved was running, attempt 1", out)
        self.assertIn(
            "escalated to the policy after 1 recovery attempt(s)", out)

    def test_an_external_reset_in_the_guard(self):
        self.assertIn("3 failed run(s) in a row, 5 in all, the last failure external (the reset pin, "
                      "an external watchdog); runOpen", "\n".join(self.lines("health_an_external_reset")))

    def test_a_panic_in_the_guard_and_no_records_at_all(self):
        self.assertIn("the last failure panic (stack smash)",
                      "\n".join(self.lines("health_a_panic_in_the_guard")))
        self.assertEqual(self.lines("records_crash_health_no_symbols"), [])


def parser_shape(parser: argparse.ArgumentParser, path: str = "") -> dict:
    """The parser's structure as data, per subcommand path: every action's option strings, dest, nargs, default,
    const, type, choices, required, help and metavar, in declaration order - not its help text, which COLUMNS and
    Python's coloured help change."""
    out = {}
    actions = []
    for a in parser._actions:
        if isinstance(a, argparse._SubParsersAction):
            for name, sub in a.choices.items():
                out.update(parser_shape(sub, f"{path} {name}".strip()))
            actions.append({"subcommands": sorted(a.choices),
                           "dest": a.dest, "required": a.required})
            continue
        actions.append({"kind": type(a).__name__, "option_strings": a.option_strings, "dest": a.dest,
                        "nargs": a.nargs, "default": a.default, "const": a.const,
                        "type": getattr(a.type, "__name__", a.type), "choices": list(a.choices) if a.choices else None,
                        "required": a.required, "help": a.help, "metavar": a.metavar})
    out[path] = actions
    return out


class CliShape(unittest.TestCase):
    """The command line every session, script and skill types: pinned as data (test_kvasir_bench_cli.json, written
    from the hand-written parser before the registry replaced it). KVASIR_BENCH_WRITE_GOLDEN=1 writes it anew -
    only for a change of the command line that is meant."""
    GOLDEN = HERE / "test_kvasir_bench_cli.json"

    def test_the_parser_is_the_golden_one(self):
        shape = json.loads(json.dumps(
            parser_shape(kb.build_parser()), sort_keys=True))
        if os.environ.get("KVASIR_BENCH_WRITE_GOLDEN"):
            self.GOLDEN.write_text(json.dumps(
                shape, indent=1, sort_keys=True) + "\n")
        golden = json.loads(self.GOLDEN.read_text())
        self.assertEqual(len(
            golden), 23, "the top level, 19 commands, printer and its start / stop / status")
        for path in sorted(set(golden) | set(shape)):
            self.assertEqual(shape.get(path), golden.get(path), f"`{path}`")


class ImageOnce(unittest.TestCase):
    """Each nm run happens at most once per image, however often a command asks; the demangled table is never
    built for the function table (profile)."""
    NM = ("10000100 00000010 T Kvasir::Panic::raise(Kvasir::Panic::Cause)\n"
          "20000000 00000008 B Kvasir::Ubsan::ubsanReports\n"
          "20000010 00000004 D Kvasir::Fault::lastFault\n"
          "20000400 b _LINKER_stack_start_\n"
          "20001400 B _LINKER_stack_end_\n")

    def image(self, asked):
        return image.Image(pathlib.Path("t.elf"), run_nm=lambda elf, demangle: asked.append(demangle) or self.NM)

    def test_one_demangled_run_for_every_lookup(self):
        asked = []
        img = self.image(asked)
        for _ in range(4):   # crash asked four times
            self.assertEqual(img.exact("Kvasir::Ubsan::ubsanReports"),
                             (0x20000000, 8, "Kvasir::Ubsan::ubsanReports"))
        self.assertIsNone(img.exact("Kvasir::Ubsan"), "exact is exact")
        self.assertEqual(img.one("lastFault$").address, 0x20000010)
        self.assertEqual(img.any_exact(
            "_LINKER_stack_start_").address, 0x20000400, "no size, any type")
        self.assertEqual(img.any_exact(
            "Kvasir::Panic::raise(Kvasir::Panic::Cause)", "tTwW").address, 0x10000100)
        self.assertIsNone(img.any_exact(
            "Kvasir::Ubsan::ubsanReports", "tTwW"), "of the types asked for")
        self.assertEqual(asked, [True])
        self.assertEqual([s.name for s in img.objects], [
                         "Kvasir::Ubsan::ubsanReports", "Kvasir::Fault::lastFault"])

    def test_the_function_table_reads_only_the_mangled_table(self):
        asked = []
        img = self.image(asked)
        self.assertEqual(img.functions, img.functions)
        self.assertEqual(asked, [False])

    def test_code_ranges_from_the_sections(self):
        # SHF_ALLOC | SHF_EXECINSTR at the run address: a RAM function's section counts where it runs
        with tempfile.TemporaryDirectory() as d:
            elf = pathlib.Path(d, "t.elf")
            elf.write_bytes(tiny_elf([(".text", 0x6, 0x10000000, 0x10000000, b"code"),
                                      (".data", 0x3, 0x20000000,
                                       0x10000004, b"data"),
                                      (".ramfunc", 0x6, 0x20000100, 0x10000008, b"fast")]))
            img = image.Image(elf)
            self.assertEqual(img.code_ranges, [
                             (0x10000000, 0x10000004), (0x20000100, 0x20000104)])
            self.assertTrue(img.is_code(0x10000003))
            self.assertFalse(img.is_code(0x20000000))
            self.assertTrue(img.is_code(0x20000101), "a Thumb address")


class SessionLazy(unittest.TestCase):
    """A Session asks for the printer when a command first needs it, and once."""

    def test_no_printer_until_asked_and_then_once(self):
        with unittest.mock.patch.object(printer, "need_printer", return_value=pathlib.Path("sock")) as need:
            s = session.Session(types.SimpleNamespace(
                build="b", target="t"), "the peek")
            need.assert_not_called()
            self.assertEqual(s.control, pathlib.Path("sock"))
            self.assertEqual(s.control, pathlib.Path("sock"))
            need.assert_called_once_with(s.tree, "the peek")

    def test_the_build_check_finds_the_printer_for_the_firmware_check(self):
        with unittest.mock.patch.object(printer, "need_printer", return_value=pathlib.Path("sock")) as need, \
                unittest.mock.patch.object(printer, "check_build") as check:
            s = session.Session(types.SimpleNamespace(
                build="b", target="t"), "the UB counter")
            s.check_build(True)
            self.assertEqual(s.control, pathlib.Path("sock"))
            need.assert_called_once_with(s.tree, "the firmware check")
            check.assert_called_once_with(s.tree, pathlib.Path("sock"), True)

    def test_guards_in_order_before_the_command(self):
        calls = []
        cmd = session.Command(("x",), lambda s, a: calls.append(
            "run"), [], True, "x", "reset", True)
        with unittest.mock.patch.object(trees, "Tree", return_value=types.SimpleNamespace()), \
                unittest.mock.patch.object(session.Session, "confirm", lambda s, a, c: calls.append(("confirm", a, c))), \
                unittest.mock.patch.object(session.Session, "check_build", lambda s, ok: calls.append(("build", ok))):
            session.run(cmd, types.SimpleNamespace(
                build="b", target="t", confirm=True, stale_ok=False))
        self.assertEqual(
            calls, [("confirm", "reset", True), ("build", False), "run"])


class SnapshotRelease(unittest.TestCase):
    """snapshot halts, reads the registers and lets the core go on the session's one connection; when anything in
    between fails, `go` is still sent - on a connection of its own, the session's may be the one that broke."""

    def run_snapshot(self, gdb_error=None):
        asked, separate = [], []

        def ask(_s, req, timeout=None):
            asked.append(req["cmd"])
            return {"halt": {"halted": True}, "registers": {"values": list(range(20))}}.get(req["cmd"], {})

        @contextlib.contextmanager
        def stub(control, registers):
            self.assertEqual(registers, list(range(17)))
            yield 1234

        def run_gdb_script(*_):
            if gdb_error:
                raise gdb_error
        with unittest.mock.patch.object(session.Session, "ask", ask), \
                unittest.mock.patch.object(gdb_stub, "printer_gdb_stub", stub), \
                unittest.mock.patch.object(gdb_stub, "run_gdb_script", run_gdb_script), \
                unittest.mock.patch.object(commands, "request", lambda c, r, **_: separate.append(r["cmd"]) or {}), \
                contextlib.redirect_stdout(io.StringIO()):
            try:
                commands.snapshot(fake_session(control=Path(
                    "sock")), types.SimpleNamespace(var=[], frames=5))
            except RuntimeError:
                pass
        return asked, separate

    def test_one_connection(self):
        self.assertEqual(self.run_snapshot(),
                         (["halt", "registers", "go"], []))

    def test_go_after_a_failed_gdb_run(self):
        self.assertEqual(self.run_snapshot(RuntimeError(
            "gdb timed out")), (["halt", "registers"], ["go"]))


class PluginContract(unittest.TestCase):
    """A chip plugin's `run(bench, args)` gets the Session: bench.read([a]) -> (stamp, [word]), bench.write([(a, v)])
    -> [word read back], on one connection to the printer."""

    def test_read_and_write_words(self):
        with tempfile.TemporaryDirectory() as d:
            path = pathlib.Path(d) / "control.sock"
            server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            server.bind(str(path))
            server.listen(1)
            asked = []

            def printer_side():
                conn, _ = server.accept()
                with conn, conn.makefile("rw") as f:
                    for line in f:
                        req = json.loads(line)
                        asked.append(req)
                        word = (0x12345678).to_bytes(4, "little").hex()
                        f.write(json.dumps({"cmd": req["cmd"], "unix_us": 77,
                                            "data": [word] * len(req.get("pieces") or req.get("words"))}) + "\n")
                        f.flush()

            thread = threading.Thread(target=printer_side, daemon=True)
            thread.start()
            with fake_session(control=path) as bench:
                self.assertEqual(bench.read([0x40000000]), (77, [0x12345678]))
                self.assertEqual(bench.write([(0x40000004, 5)]), [0x12345678])
            thread.join(timeout=5)
            server.close()
            self.assertEqual([r["cmd"] for r in asked], ["read", "write"])


class CrashOutput(unittest.TestCase):
    """`crash` on a halt: the register lines, the stack's code words, the records, then this run's COREFAULT line -
    from the printer's history directly (it ran `kvasir_bench.py log` as a second process before), the same text."""
    HALT = ("core halted: exception=3 pc=0x10000124 lr=0x10000201 sp=0x20001fd0 xpsr=0x61000003 "
            "stack: 20001000 10000201 00000007 10000125")

    def run_crash(self, history_lines, history_error=None, halt_kind="halt"):
        tree = types.SimpleNamespace(build="b", target="t")
        s = fake_session(tree, control=pathlib.Path("sock"))
        s.tree.image = types.SimpleNamespace(
            is_code=lambda a: 0x10000000 <= (a & ~1) < 0x10001000)
        s.records = lambda kinds: fixture_answer("records_crash")
        asked = []

        def history(control, **filters):
            asked.append(filters)
            if history_error:
                raise history_error
            return history_lines, {"cmd": "backlog_end"}
        out = io.StringIO()
        with unittest.mock.patch.object(commands, "request", return_value={"messages": [
            {"level": "info", "text": "core running", "time": "12:00:00.000"},
            {"level": "error", "text": self.HALT, "time": "12:00:01.500"} | (
                {"kind": halt_kind} if halt_kind else {})]}), \
                unittest.mock.patch.object(commands, "addr2line", lambda tree, addrs: {
                    a: f"fn_{a:x}()  src/x.cpp:{a & 0xff}" for a in addrs}), \
                unittest.mock.patch.object(records, "full_records", lambda s, answer: print("[records]")), \
                unittest.mock.patch.object(records, "print_health_records", lambda s, answer: print("[health]")), \
                unittest.mock.patch.object(commands, "history", history), \
                unittest.mock.patch.object(printer, "mark_seq", return_value=41), \
                contextlib.redirect_stdout(out):
            commands.crash(s, types.SimpleNamespace())
        return out.getvalue(), asked

    def event(self, ms, message):
        return {"uc_time_ns": int(ms * 1e6), "level": "crit", "module": "", "file": "f.cpp", "line": 3,
                "message": message}

    def test_the_text(self):
        fault = self.event(10, "COREFAULT " + "x" * 700)
        text, asked = self.run_crash(
            [self.event(5, "COREFAULT older"), self.event(9, "other"), fault])
        lines = text.splitlines()
        self.assertEqual(lines[:9], [
            "core halted at 12:00:01.500 in HardFault",
            "  pc    0x10000124  fn_10000124()  src/x.cpp:36",
            "  lr    0x10000201  fn_10000200()  src/x.cpp:0",
            "  sp    0x20001fd0",
            "  xpsr  0x61000003",
            "  words on the stack that point into code (return addresses, the stacked pc/lr - "
            "Kvasir's fault handler moves sp, so the faulting frame is in its own log line):",
            "    sp+4   0x10000201  fn_10000200()  src/x.cpp:0",
            "    sp+12  0x10000125  fn_10000124()  src/x.cpp:36",
            "[records]"])
        self.assertEqual(lines[9], "[health]")
        self.assertEqual(len(lines), 11, "the last COREFAULT line only")
        # what `log --grep COREFAULT --since-mark --last 1 --width 600` printed, stripped
        self.assertEqual(lines[10], "  log: " +
                         uc_log_client.log_line_text(fault)[:600].strip())
        self.assertEqual(asked, [{"min_level": None, "modules": None, "not_modules": None, "since_seq": 41}],
                         "since this tool's last mark, filtered here")

    def test_the_exception_names_come_from_the_printer(self):
        """0-15 by the answer's exception_names (the "IRQ -N" text of 1, 7, 12 is gone), 16 and up as IRQ n - 16."""
        for exception, name in ((0, "thread mode (a breakpoint or a debugger halted it)"), (1, "exception 1"),
                                (7, "SecureFault"), (12, "DebugMonitor"), (15, "SysTick"), (20, "IRQ 4")):
            with self.subTest(exception=exception):
                self.HALT = f"core halted: exception={exception} pc=0x10000124 lr=0x10000201"
                text, _ = self.run_crash([])
                self.assertEqual(text.splitlines()[
                                 0], f"core halted at 12:00:01.500 in {name}")

    def test_a_record_that_could_not_be_read_stops_it(self):
        """A failed read is no empty record: before the printer decoded them a refused read died, and still does."""
        for halt in (True, False):
            with self.subTest(halt=halt):
                s = fake_session(control=pathlib.Path("sock"))
                s.tree.image = types.SimpleNamespace(is_code=lambda a: False)
                s.records = lambda kinds: {"fault": {"address": 0x20000000, "valid": False, "error": "probe gone"},
                                           "panic_causes": [], "exception_names": ["thread mode"] * 16}
                messages = [{"level": "error", "text": self.HALT, "time": "12:00:01.500", "kind": "halt"}] if halt \
                    else []
                err, out = io.StringIO(), io.StringIO()
                with unittest.mock.patch.object(commands, "request", return_value={"messages": messages}), \
                        unittest.mock.patch.object(commands, "addr2line", return_value={}), \
                        contextlib.redirect_stdout(out), contextlib.redirect_stderr(err), \
                        self.assertRaises(SystemExit) as exit_:
                    commands.crash(s, types.SimpleNamespace())
                self.assertEqual(exit_.exception.code, 1)
                self.assertEqual(err.getvalue(), "kvasir_bench: the fault record at 0x20000000 could not be read "
                                                 "through the printer: probe gone\n")
                # the halt's own lines come from the printer's message, not from a read: they are still printed
                self.assertEqual(
                    "core halted at 12:00:01.500 in thread mode" in out.getvalue(), halt)

    def test_a_plain_error_with_the_words_is_no_halt(self):
        text, _ = self.run_crash([], halt_kind=None)
        self.assertEqual(
            text, "no 'core halted' message: the printer did not see the core stop\n[records]\n[health]\n")

    def test_an_unreadable_history_ends_after_the_records(self):
        from uc_log_client import ControlError
        text, _ = self.run_crash(
            [], history_error=ControlError("unknown command"))
        self.assertTrue(text.endswith("[records]\n[health]\n"), text)


class ChunkedReads(unittest.TestCase):
    """Session.read_bytes: every request as large as the printer takes (READ_BYTES), all on the session's one
    connection; the gdb stub's reads answer None for a refused one."""

    def test_one_connection_largest_requests(self):
        asked = []

        def ask(req):
            asked.append(req["pieces"])
            (address, size), = [(p[0], p[1]) if isinstance(p, list) else (p["address"], p["size"])
                                for p in req["pieces"]]
            return {"cmd": "read", "unix_us": 1, "data": [bytes(size).hex()]}
        s = fake_session(control=pathlib.Path("sock"))
        with unittest.mock.patch.object(session.Session, "ask", lambda self, req: ask(req)):
            self.assertEqual(len(s.read_bytes(0x20000000, 2 * uc_log_client.READ_BYTES + 10)),
                             2 * uc_log_client.READ_BYTES + 10)
        self.assertEqual(len(asked), 3)

    def test_a_refused_read_is_none_for_the_stub(self):
        from uc_log_client import ControlError

        def ask(req):
            raise ControlError("not readable")
        self.assertIsNone(printer.read_chunked(ask, 0, 8, on_error="none"))
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            printer.read_chunked(ask, 0, 8)


class ToolLookup(unittest.TestCase):
    def test_llvm_first_then_gnu_then_plain_and_none_is_said(self):
        for present, want in ((["llvm-nm", "arm-none-eabi-nm", "nm"], "/bin/llvm-nm"),
                              (["arm-none-eabi-nm", "nm"], "/bin/arm-none-eabi-nm"), (["nm"], "/bin/nm")):
            with unittest.mock.patch.object(shutil, "which", lambda n: f"/bin/{n}" if n in present else None):
                self.assertEqual(image.tool("nm"), want)
        err = io.StringIO()
        with unittest.mock.patch.object(shutil, "which", return_value=None), contextlib.redirect_stderr(err), \
                self.assertRaises(SystemExit):
            image.tool("addr2line")
        self.assertIn(
            "no llvm-addr2line / arm-none-eabi-addr2line / addr2line in PATH", err.getvalue())


class ChipStartsAPrinter(unittest.TestCase):
    """A chip command with no printer running starts one, as every command does (and it stops at the end)."""

    def test_the_plugin_gets_a_started_printer(self):
        chip = next(c for c in session.REGISTRY if c.path == ("chip",))
        plugin = types.SimpleNamespace(commands=lambda: {"xip": ("x", lambda p: None,
                                                                 lambda bench, a: print(bench.control))})
        with unittest.mock.patch.object(trees, "Tree", return_value=types.SimpleNamespace(build="b", target="t")), \
                unittest.mock.patch.object(commands, "load_chip_plugin", return_value=plugin), \
                unittest.mock.patch.object(printer, "need_printer", return_value=pathlib.Path("sock")) as need, \
                contextlib.redirect_stdout(io.StringIO()) as out:
            session.run(chip, types.SimpleNamespace(
                build="b", target="t", rest=["xip"]))
        need.assert_called_once_with(unittest.mock.ANY, "the chip command")
        self.assertEqual(out.getvalue(), "sock\n")


class RecordConstantsPinned(unittest.TestCase):
    """The host copies of what the firmware writes for a debugger that the printer does not decode (coverage), held to
    the SDK's sources, and the build id held to the printer's firmware check."""
    SRC = HERE.parent / "src" / "kvasir"

    def constant(self, header, pattern):
        match = re.search(pattern, (self.SRC / header).read_text())
        self.assertIsNotNone(match, f"{pattern} in {header}")
        return int(match.group(1).replace("'", ""), 0)

    def test_coverage_manifest(self):
        self.assertEqual(coverage_data.COVERAGE_MAGIC,
                         self.constant("Util/Coverage.hpp", r"constexpr std::uint32_t Magic = ([0-9xA-Fa-f']+)U"))
        self.assertEqual(coverage_data.COVERAGE_PIECES, self.constant(
            "Util/Coverage.hpp", r"Piece\s+pieces\[(\d+)\]"))

    def test_hex_build_id_is_the_printers(self):
        """uc_log tests/hex_image_tests.cpp's fixture: its imageCrc there is the same literal"""
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / "t_flash.hex").write_text(
                ":020000041000EA\n:0400000001020304F2\n:0400040005060708DE\r\n:020000042000DA\n:02000000AABB99\n"
                ":0400000510000101E5\n:00000001FF\n")
            self.assertEqual(trees.hex_build_id(
                types.SimpleNamespace(build=Path(tmp), target="t")), "1c6c3b4b")


class FixtureTarget:
    """uc_log's fixture target (tests/data/records) as a command sees a board: the image (code where fixture.json
    says, its resident bytes = its flash regions, for addr2line and the check names) and the printer that answers a
    records request with the fixture case's answer (records_fixture.jsonl)."""

    def __init__(self):
        fixture = json.loads((FIXTURE / "fixture.json").read_text())
        code = (fixture["code"]["start"], fixture["code"]["end"])
        flash = [(".rodata", m["address"], bytes.fromhex(m["hex"])) for m in fixture["memory"]
                 if m["address"] < 0x20000000]
        self.image = types.SimpleNamespace(is_code=lambda address: code[0] <= (address & ~1) < code[1],
                                           resident=lambda: flash)

    def session(self, case: str):
        return types.SimpleNamespace(tree=types.SimpleNamespace(build="b", target="t"), image=self.image,
                                     control=pathlib.Path("sock"), records=lambda kinds: fixture_answer(case))


def run_text(fn) -> dict:
    """What a command prints: stdout and stderr lines, its exit code."""
    out, err, code = io.StringIO(), io.StringIO(), 0

    def fake_addr2line(_tree, addresses):
        return {a: f"fn_{a:08x}()  src/fixture.cpp:{a & 0xff}" for a in addresses}
    with unittest.mock.patch.object(records, "addr2line", fake_addr2line), \
            unittest.mock.patch.object(commands, "addr2line", fake_addr2line), \
            contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        try:
            fn()
        except SystemExit as e:
            code = e.code
    return {"stdout": out.getvalue().splitlines(), "stderr": err.getvalue().splitlines(), "exit": code}


class RecordTextGoldens(unittest.TestCase):
    """The text of crash's records, stack, ub, trace and panic's requests over uc_log's fixture target, written from
    the per-record Python decoders before the printer became the one decoder (step 149 LG6 P0): the same text must
    come out of the `records` / `raise_panic` answers. test_kvasir_bench_records.json; KVASIR_BENCH_WRITE_RECORD_GOLDEN=1
    writes it anew - only for a text change that is meant."""
    GOLDEN = HERE / "test_kvasir_bench_records.json"

    @staticmethod
    def crash_records(case):
        s = FixtureTarget().session(case)
        answer = fixture_answer(case)
        return run_text(lambda: (records.full_records(s, answer), records.print_health_records(s, answer)))

    @staticmethod
    def command(which, case, **options):
        fn = {"stack": commands.stack, "ub": commands.ub,
              "trace": commands.trace}[which]
        args = types.SimpleNamespace(stale_ok=False, **options)
        return run_text(lambda: fn(FixtureTarget().session(case), args))

    @staticmethod
    def trace(case, name=None, last=0, hex=False, delta=False):
        return RecordTextGoldens.command("trace", case, name=name, last=last, hex=hex, delta=delta)

    @staticmethod
    def panic(cause):
        sent = []
        s = fake_session(control=pathlib.Path("sock"))
        s.ask = lambda req, timeout=None: sent.append(
            req) or {"cmd": "raise_panic"}
        with unittest.mock.patch.object(printer, "set_mark", lambda *_: None):
            text = run_text(lambda: commands.panic(
                s, types.SimpleNamespace(cause=cause)))
        return text | {"requests": sent}

    def cases(self) -> dict:
        return {
            "crash_records": self.crash_records("records_lld"),
            "crash_records_empty": self.crash_records("records_crash_health_empty"),
            "crash_records_no_symbols": self.crash_records("records_crash_health_no_symbols"),
            "stack_painted": self.command("stack", "records_lld"),
            "stack_not_painted": self.command("stack", "records_stacks_not_painted"),
            "stack_no_symbols": self.command("stack", "records_stacks_no_symbols"),
            "ub_reports": self.command("ub", "records_lld"),
            "ub_none": self.command("ub", "records_ubsan_none"),
            "ub_not_sanitize": self.command("ub", "records_ubsan_not_sanitize"),
            "trace_isr": self.trace("records_lld", "isr"),
            "trace_edges": self.trace("records_lld", "edges"),
            "trace_last_5": self.trace("records_lld", last=5),
            "trace_edges_delta_last_8": self.trace("records_lld", "edges", last=8, delta=True),
            "trace_edges_hex_last_5": self.trace("records_lld", "edges", last=5, hex=True),
            "trace_written_while_reading": self.trace("records_trace_written_while_reading", "edges", last=6),
            # 6 more while reading: the records 0..2 may be overwritten, they are dropped
            "trace_written_while_reading_drops": self.trace("records_trace_written_while_reading_drops", "isr"),
            "trace_no_ring_of_that_name": self.trace("records_lld", "nope"),
            "trace_no_rings": self.trace("records_trace_no_rings"),
            "trace_a_storage_without_its_descriptor": self.trace("records_trace_a_storage_without_its_descriptor"),
            "panic_check_failed": self.panic("check-failed"),
            "panic_user": self.panic("user"),
        }

    def test_the_text_is_the_golden_one(self):
        cases = json.loads(json.dumps(self.cases()))
        if os.environ.get("KVASIR_BENCH_WRITE_RECORD_GOLDEN"):
            self.GOLDEN.write_text(json.dumps(
                cases, indent=1, sort_keys=True) + "\n")
        golden = json.loads(self.GOLDEN.read_text())
        for name in sorted(set(golden) | set(cases)):
            self.assertEqual(cases.get(name), golden.get(name), name)

    def test_the_fixture_is_what_its_readme_says(self):
        # a few anchors of the fixture, so a regenerated one that drifts is seen here and not only as golden noise
        answer = fixture_answer("records_lld")
        self.assertEqual(answer["panic"]["record"]["cause"], 7)
        self.assertEqual([r["name"]
                         for r in answer["rings"]], ["edges", "isr"])
        self.assertEqual(answer["rings"][0]["count"], 1500)


if __name__ == "__main__":
    unittest.main()
