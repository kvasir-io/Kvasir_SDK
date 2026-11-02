#!/usr/bin/env python3
"""kvasir_test.py without a board: the image's list from an nm fixture, the run state's decode, the request, and whole
runs against a fake board that answers like the runner (Kvasir_SDK/src/kvasir/Test) - on a fake clock."""
import io
import sys
import types
import unittest
import xml.etree.ElementTree as ET
import zlib
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import kvasir_test as kt  # noqa: E402

NM = (HERE / "test_kvasir_test_nm.txt").read_text()
REQ, RUN = 0x4B2BC, 0x4B240   # the fixture's kvasir_test_request / kvasir_test_run
# the build id of the image under test (Options.build_id), as READY prints it
BUILD = "0123456789abcdef"
READY3 = f"TEST|READY|n=3|seq=0|build={BUILD}|wait=3000ms"


def encode_state(seq, verdicts, *, selected=None, flags=1, boots=1, phase=2, in_flight=0, nxt=None):
    """A Persistent<RunState, RunTag> as the target stores it (RunState.hpp)."""
    count = len(verdicts)
    selected = set(range(count)) if selected is None else selected
    code = {"none": 0, "pass": 1, "fail": 2, "skip": 3}
    vw, sw = [0] * 16, [0] * 8
    for i, v in enumerate(verdicts):
        vw[i // 16] |= code[v] << (2 * (i % 16))
    for i in selected:
        sw[i // 32] |= 1 << (i % 32)
    c = {k: sum(1 for v in verdicts if v == k)
         for k in ("pass", "fail", "skip")}
    nxt = count if nxt is None else nxt
    words = [kt.RUN_ID, seq, nxt | in_flight << 16, c["pass"] | c["fail"] << 16, c["skip"] | boots << 16,
             phase | flags << 8 | count << 16, *vw, *sw]
    data = b"".join(w.to_bytes(4, "little") for w in words)
    return data + zlib.crc32(data).to_bytes(4, "little")


def log(text):
    return {"cmd": "log", "message": text, "level": "info"}


class Clock:
    def __init__(self):
        self.t = 0.0

    def __call__(self):
        return self.t


class FakeBoard:
    """Answers like a Kvasir::Test image: READY after a flash; a written request starts its scripted run (`lines` for
    a seq, the state it leaves). `takes_request` False: the image ignores the request until a reset."""

    def __init__(self, clock, run_lines, verdicts, *, takes_request=True, after_flash=None, extra_state=None):
        self.clock, self.run_lines, self.verdicts = clock, run_lines, verdicts
        self.takes_request, self.after_flash = takes_request, after_flash
        self.extra_state = extra_state or {}
        # kvasir_test_run at 0, kvasir_test_request at 0x80
        self.memory = bytearray(0x100)
        self.pending, self.resets, self.flashes, self.writes = [], 0, 0, []
        self.ub = 0
        self.firmware = "match"
        self.busy = 0   # the next accesses that meet an announced reset

    def _at(self, address):
        return 0x80 + address - REQ if address >= REQ else address - RUN

    def build(self):
        pass

    def flash(self):
        self.flashes += 1
        self.pending += self.after_flash if self.after_flash is not None else [log("boot: v1 reset cause: x"),
                                                                               log(READY3)]
        return 0

    def mark_now(self):
        return 0

    def reset(self):
        self.resets += 1
        self.takes_request = True
        self._run()

    def events(self, since):
        while True:
            if self.pending:
                self.clock.t += 0.001
                yield self.pending.pop(0)
            else:
                self.clock.t += 0.5
                yield None

    def pause(self):
        self.clock.t += 0.25

    def _away(self):
        if self.busy:
            self.busy -= 1
            raise kt.BoardBusy("target paused for an announced reset")

    def read(self, address, size):
        self._away()
        a = self._at(address)
        return bytes(self.memory[a:a + size])

    def write(self, writes):
        self._away()
        self.writes.append(writes)
        for address, value in writes:
            a = self._at(address)
            self.memory[a:a + 4] = value.to_bytes(4, "little")
        if self.takes_request:
            self._run()

    def _run(self):
        seq = int.from_bytes(self.memory[0x84:0x88], "little")
        self.pending += [log(l.replace("{seq}", str(seq)))
                         for l in self.run_lines]
        state = encode_state(seq, self.verdicts, **self.extra_state)
        self.memory[0:len(state)] = state

    def ub_count(self):
        return self.ub

    def firmware_state(self):
        return self.firmware


ALL_PASS = ["TEST|START|seq={seq}|n=3|of=3", "TEST|PASS|0|self.ok|12us", "TEST|EXPECT|1|1 > 2 failed: 1 vs 2",
            "TEST|FAIL|1|self.bad|1 failed|30us", "TEST|PASS|2|self.third|5us",
            "TEST|DONE|pass=2|fail=1|skip=0|boots=1|seq={seq}"]


def run(board, clock, **opts):
    out = []
    with redirect_stdout(io.StringIO()):
        try:
            code, runs = kt.run_tests(board, NM, kt.Options(
                **({"build_id": BUILD} | opts)), now=clock, out=out.append)
        except kt.Outcome as o:
            return o.code, None, out + [str(o)]
    return code, runs, out


class ImageList(unittest.TestCase):
    def test_listed_and_defined(self):
        tests, defined = kt.tests_in(NM)
        self.assertEqual([t.label for t in tests], [
                         "self.ok", "self.bad", "self.third"])
        self.assertEqual([t.index for t in tests], [0, 1, 2])
        self.assertEqual(kt.unlisted(tests, defined),
                         [("unlisted", "forgotten")])

    def test_user_namespace(self):
        nm = ("10001000 00000010 t _ZN6Kvasir4Test6Detail4SlotILj0EN2ns11KvasirTests3bus5probeEE3runERNS0_9PerfStateE\n"
              "10001100 00000010 t _ZN2ns11KvasirTests3bus5probe4bodyERN6Kvasir4Test7ContextE\n")
        tests, defined = kt.tests_in(nm)
        self.assertEqual(tests, [kt.TestId(0, "bus", "probe")])
        self.assertEqual(kt.unlisted(tests, defined), [])

    def test_symbols(self):
        self.assertEqual(kt.symbol_address(NM, "kvasir_test_request"), REQ)
        self.assertEqual(kt.symbol_address(NM, "kvasir_test_run"), RUN)

    def test_select(self):
        tests, _ = kt.tests_in(NM)
        self.assertEqual(kt.select(tests, [], []), {0, 1, 2})
        self.assertEqual(kt.select(tests, ["self.t*"], []), {2})
        self.assertEqual(kt.select(tests, [], ["*.bad"]), {0, 2})


class Words(unittest.TestCase):
    def test_request_order_magic_last(self):
        words = kt.request_words(7, kt.STOP_AT_FIRST_FAILURE, {0, 33})
        self.assertEqual(len(words), 11)
        self.assertEqual(words[:4], [0x5152544B, 7, 1, 1])
        self.assertEqual(words[4], 2)
        writes = kt.request_writes(0x2000_0000, words)
        # one write: the magic cleared, words 1..10, the magic last
        self.assertEqual(writes[0], (0x2000_0000, 0))
        self.assertEqual([a for a, _ in writes[1:-1]],
                         [0x2000_0000 + 4 * i for i in range(1, 11)])
        self.assertEqual(writes[1], (0x2000_0004, 7))
        self.assertEqual(writes[-1], (0x2000_0000, kt.REQUEST_MAGIC))

    def test_state_decode(self):
        data = encode_state(5, ["pass", "fail", "skip", "none"], selected={
                            0, 1, 2}, boots=3, in_flight=0)
        s = kt.decode_run_state(data)
        self.assertEqual((s.request_seq, s.count, s.boots,
                         s.passed, s.failed, s.skipped), (5, 4, 3, 1, 1, 1))
        self.assertEqual(s.verdicts, ["pass", "fail", "skip", "none"])
        self.assertEqual(s.selected, {0, 1, 2})
        self.assertEqual(s.served_seq(), 5)
        bad = bytearray(data)
        bad[8] ^= 1
        self.assertIsNone(kt.decode_run_state(bytes(bad)))
        self.assertIsNone(kt.decode_run_state(b"\0" * 124))


class Runs(unittest.TestCase):
    def test_all_lines(self):
        clock = Clock()
        board = FakeBoard(clock, ALL_PASS, ["pass", "fail", "pass"])
        code, runs, out = run(board, clock)
        self.assertEqual(code, kt.EXIT_FAIL)
        r = runs[0]
        self.assertEqual([x.verdict for x in r.selected()],
                         ["pass", "fail", "pass"])
        self.assertEqual(r.tests[1].expects, ["1 > 2 failed: 1 vs 2"])
        self.assertTrue(any("2 passed, 1 failed, 0 skipped" in l for l in out))
        self.assertTrue(any("defined but not listed" in l for l in out))
        self.assertEqual(board.resets, 0)
        # the request: words 1.. then the magic, a seq that the state had not taken
        self.assertEqual(board.writes[0][-1][1], kt.REQUEST_MAGIC)
        self.assertEqual(board.writes[0][0][1], 0)
        self.assertEqual(board.writes[0][1][1], 1)

    def test_all_pass_exit_0_and_junit(self):
        clock = Clock()
        lines = [l for l in ALL_PASS if "EXPECT" not in l and "FAIL" not in l] + []
        lines.insert(2, "TEST|SKIPPED|1|no part")
        lines.insert(3, "TEST|SKIP|1|self.bad")
        board = FakeBoard(clock, lines, ["pass", "skip", "pass"])
        code, runs, _ = run(board, clock)
        self.assertEqual(code, kt.EXIT_OK)
        tree = kt.junit(runs, "x_sanitize")
        root = ET.fromstring(ET.tostring(tree.getroot()))
        suite = root.find("testsuite")
        self.assertEqual((suite.get("name"), suite.get("tests"), suite.get("skipped"), suite.get("failures")),
                         ("self", "3", "1", "0"))
        self.assertEqual(
            root.find(".//testcase[@name='bad']/skipped").get("message"), "no part")

    def test_lost_line_reconciled_from_the_state(self):
        clock = Clock()
        lines = [l for l in ALL_PASS if "PASS|2" not in l]
        board = FakeBoard(clock, lines, ["pass", "fail", "pass"])
        code, runs, out = run(board, clock)
        self.assertEqual(runs[0].tests[2].verdict, "pass")
        self.assertTrue(runs[0].tests[2].lines_lost)
        self.assertTrue(any("(lines lost)" in l for l in out))

    def test_crash_and_after(self):
        clock = Clock()
        lines = ["TEST|START|seq={seq}|n=3|of=3", "TEST|PASS|0|self.ok|3us",
                 "TEST|RESET|1|self.bad|expecting panic", "boot: v1 reset cause: watchdog_timer",
                 "TEST|AFTER|1|self.bad|pass|asExpected|panic=true|cause=assertion|pc=0x10000123",
                 "TEST|PASS|2|self.third|4us", "TEST|DONE|pass=3|fail=0|skip=0|boots=2|seq={seq}"]
        board = FakeBoard(
            clock, lines, ["pass", "pass", "pass"], extra_state={"boots": 2})
        code, runs, out = run(board, clock)
        self.assertEqual(code, kt.EXIT_OK)
        self.assertEqual(
            runs[0].tests[1].after, "asExpected panic=true|cause=assertion|pc=0x10000123")
        self.assertEqual(runs[0].boots, 2)
        self.assertTrue(any("in 2 boots" in l for l in out))

    def test_core_halted_is_exit_3(self):
        clock = Clock()
        lines = ["TEST|START|seq={seq}|n=3|of=3"]
        board = FakeBoard(clock, lines, ["none"] * 3)
        board.run_lines = lines
        orig = board._run

        def run_then_halt():
            orig()
            board.pending.append(
                {"cmd": "message", "kind": "halt", "text": "core halted: pc=0x1"})
        board._run = run_then_halt
        code, _, out = run(board, clock)
        self.assertEqual(code, kt.EXIT_TROUBLE)
        self.assertIn("core halted", out[-1])

    def test_stale_done_ignored(self):
        clock = Clock()
        lines = ["TEST|DONE|pass=3|fail=0|skip=0|boots=1|seq=99"] + ALL_PASS
        board = FakeBoard(clock, lines, ["pass", "fail", "pass"])
        code, runs, _ = run(board, clock)
        self.assertEqual(runs[0].done["seq"], 1)
        self.assertEqual(code, kt.EXIT_FAIL)

    def test_lines_of_a_run_before_ours_are_dropped(self):
        clock = Clock()
        lines = ["TEST|START|seq=0|n=3|of=3",
                 "TEST|FAIL|0|self.ok|1 failed|1us"] + ALL_PASS
        board = FakeBoard(clock, lines, ["pass", "fail", "pass"])
        _, runs, _ = run(board, clock)
        self.assertEqual(runs[0].tests[0].verdict, "pass")

    def test_no_start_resets_once(self):
        clock = Clock()
        board = FakeBoard(clock, ALL_PASS, [
                          "pass", "fail", "pass"], takes_request=False)
        code, runs, _ = run(board, clock)
        self.assertEqual(board.resets, 1)
        self.assertEqual(code, kt.EXIT_FAIL)

    def test_never_taken_is_timeout(self):
        clock = Clock()
        board = FakeBoard(clock, [], ["none"] * 3, takes_request=False)
        board.reset = lambda: setattr(board, "resets", board.resets + 1)
        code, _, out = run(board, clock)
        self.assertEqual(code, kt.EXIT_TIMEOUT)
        self.assertIn("did not take request", out[-1])

    def test_wrong_count_refused(self):
        clock = Clock()
        board = FakeBoard(clock, ALL_PASS, [
                          "pass"] * 3, after_flash=[log(f"TEST|READY|n=4|seq=0|build={BUILD}|wait=3000ms")])
        code, _, out = run(board, clock)
        self.assertEqual(code, kt.EXIT_FAIL)
        self.assertIn("lists 4 tests", out[-1])

    def test_ub_fails_the_run(self):
        clock = Clock()
        board = FakeBoard(clock, [l for l in ALL_PASS if "FAIL" not in l and "EXPECT" not in l],
                          ["pass", "pass", "pass"])
        board.ub = 2
        code, _, out = run(board, clock, sanitize=True)
        self.assertEqual(code, kt.EXIT_TROUBLE)
        self.assertTrue(any("2 UB reports" in l for l in out))

    def test_stop_at_first_failure_not_incomplete(self):
        clock = Clock()
        lines = ALL_PASS[:4] + \
            ["TEST|DONE|pass=1|fail=1|skip=0|boots=1|seq={seq}"]
        board = FakeBoard(clock, lines, ["pass", "fail", "none"])
        code, _, _ = run(board, clock, stop_at_first_failure=True)
        self.assertEqual(code, kt.EXIT_FAIL)
        self.assertEqual(board.writes[0][2][1],
                         kt.STOP_AT_FIRST_FAILURE)   # word 2: flags

    def test_incomplete_is_timeout(self):
        clock = Clock()
        lines = ALL_PASS[:2] + \
            ["TEST|DONE|pass=1|fail=0|skip=0|boots=1|seq={seq}"]
        board = FakeBoard(clock, lines, ["pass", "none", "none"])
        code, _, _ = run(board, clock)
        self.assertEqual(code, kt.EXIT_TIMEOUT)

    def test_repeat_new_seq_no_reflash(self):
        clock = Clock()
        board = FakeBoard(clock, ALL_PASS, ["pass", "fail", "pass"])
        code, runs, _ = run(board, clock, repeat=2)
        self.assertEqual(len(runs), 2)
        self.assertEqual(board.flashes, 1)
        self.assertEqual([w[1][1] for w in board.writes], [1, 2])
        self.assertEqual(
            len(kt.junit(runs, "t").getroot().findall("testsuite")), 2)

    def test_repeat_times_each_run(self):
        """Each summary line has its run's own time (the first one with its flash), not the time since the start."""
        clock = Clock()
        board = FakeBoard(clock, ALL_PASS, ["pass", "fail", "pass"])
        flash = board.flash

        def slow_flash():
            clock.t += 10
            return flash()

        board.flash = slow_flash
        _, _, out = run(board, clock, repeat=2)
        secs = [float(line.rsplit(", ", 1)[1].split()[0])
                for line in out if " boots, " in line]
        self.assertEqual(len(secs), 2)
        self.assertGreaterEqual(secs[0], 10)
        self.assertLess(secs[1], 10)


class DoneLine(unittest.TestCase):
    def done_line(self, lines, passed="fail=0"):
        clock = Clock()
        board = FakeBoard(clock, [], [], after_flash=[log(l) for l in lines])
        with redirect_stdout(io.StringIO()):
            return kt.run_done_line(board, kt.Options(timeout=10), r"CRIT\|DONE", passed, now=clock, out=lambda _: None)

    def test_pass(self):
        self.assertEqual(self.done_line(
            ["x", "CRIT|DONE|ok=5|fail=0"])[0], kt.EXIT_OK)

    def test_fail(self):
        code, line = self.done_line(["CRIT|DONE|ok=4|fail=1"])
        self.assertEqual(code, kt.EXIT_FAIL)
        root = kt.done_line_junit("t", line, code).getroot()
        self.assertIsNotNone(root.find(".//failure"))

    def test_timeout(self):
        self.assertEqual(self.done_line(["CRIT|STEP|1"])[0], kt.EXIT_TIMEOUT)


class NoBoard(unittest.TestCase):
    def test_probe_matching(self):
        usb = [("11111111", "J-Link A", "A"), ("22222222", "J-Link", "")]
        self.assertTrue(kt.probe_present("A", "", usb))
        self.assertTrue(kt.probe_present("022222222", "", usb))
        self.assertFalse(kt.probe_present("B", "", usb))
        self.assertTrue(kt.probe_present("B", "192.0.2.1", []))
        self.assertTrue(kt.probe_present("", "", usb))
        self.assertFalse(kt.probe_present("", "", []))

    def test_exit_77_before_anything(self):
        class FakeImage:
            mangled_nm = NM

        class FakeTree:
            def __init__(self, build, target):
                self.probe, self.host, self.image = "nobody", "", FakeImage()

        orig = kt.trees.Tree
        kt.trees.Tree = FakeTree
        try:
            with redirect_stderr(io.StringIO()) as err:
                code = kt.main(["run", "b", "t_sanitize"], usb=lambda: [])
        finally:
            kt.trees.Tree = orig
        self.assertEqual(code, kt.EXIT_NO_BOARD)
        self.assertIn("not on USB", err.getvalue())

    def test_list_exit_1_on_unlisted(self):
        class FakeImage:
            mangled_nm = NM

        class FakeTree:
            def __init__(self, build, target):
                self.image = FakeImage()

        orig = kt.trees.Tree
        kt.trees.Tree = FakeTree
        try:
            with redirect_stdout(io.StringIO()) as out:
                code = kt.main(["list", "b", "t"])
        finally:
            kt.trees.Tree = orig
        self.assertEqual(code, kt.EXIT_FAIL)
        self.assertIn(
            "unlisted.forgotten is defined but not listed", out.getvalue())


class ArmImage(unittest.TestCase):
    """The nm of two clang test images of the same list: RP2350 (test_kvasir_test_nm_arm.txt) and RP2040
    (test_kvasir_test_nm_arm_m0.txt, where Dwt.Counts is disabled - its slot must be there all the same): eight tests
    listed in order, every body there, none unlisted, the two C names found."""

    def test_list(self):
        for fixture in ("test_kvasir_test_nm_arm.txt", "test_kvasir_test_nm_arm_m0.txt"):
            with self.subTest(fixture=fixture):
                self.check((HERE / fixture).read_text())

    def check(self, nm):
        tests, defined = kt.tests_in(nm)
        self.assertEqual([t.label for t in tests],
                         ["Rollover.Wraps", "Timing.ClockAdvances", "Bench.HasMagnet", "Panic.CheckFailed",
                          "Fault.BusLoad", "Watchdog.Fires", "Memcpy.OneKib", "Dwt.Counts"])
        self.assertEqual([t.index for t in tests], list(range(8)))
        self.assertEqual(kt.unlisted(tests, defined), [])
        self.assertIsNotNone(kt.symbol_address(nm, "kvasir_test_request"))
        self.assertIsNotNone(kt.symbol_address(nm, "kvasir_test_run"))


def note(words):
    return b"".join(w.to_bytes(4, "little") for w in words)


class ImageBuildId(unittest.TestCase):
    """The READY line's build id against the ELF's GNU build-id note: a stale image is refused without a flash compare;
    the READY line's host wait sets how long the host waits for its request to be taken."""

    def test_note_bytes(self):
        good = note([4, 20, 3, 0x0055_4E47, 0x6789ABCD, 0x01234567])
        self.assertEqual(kt.build_id_of(good), "6789abcd01234567")
        self.assertIsNone(kt.build_id_of(
            note([4, 20, 1, 0x0055_4E47, 1, 2])))   # another note type
        # a hash shorter than 8 bytes
        self.assertIsNone(kt.build_id_of(note([4, 4, 3, 0x0055_4E47, 1, 2])))
        self.assertIsNone(kt.build_id_of(good[:20]))
        self.assertIsNone(kt.build_id_of(None))

    def test_stale_image_after_a_flash_refused(self):
        clock = Clock()
        board = FakeBoard(clock, ALL_PASS, ["pass"] * 3,
                          after_flash=[log("TEST|READY|n=3|seq=0|build=ffffffffffffffff|wait=3000ms")])
        code, _, out = run(board, clock)
        self.assertEqual(code, kt.EXIT_REFUSED)
        self.assertIn("another image", out[-1])
        self.assertEqual(board.writes, [])   # refused before the request

    def test_stale_image_without_a_flash_refused(self):
        clock = Clock()
        lines = ["TEST|READY|n=3|seq=0|build=ffffffffffffffff|wait=3000ms"] + ALL_PASS
        board = FakeBoard(clock, lines, ["pass"] * 3)
        code, _, out = run(board, clock, flash=False)
        self.assertEqual(code, kt.EXIT_REFUSED)

    def test_printer_says_another_image(self):
        """The printer does not decode a stale image's log: its firmware check refuses it before any request."""
        clock = Clock()
        board = FakeBoard(clock, ALL_PASS, ["pass"] * 3)
        board.firmware = "different"
        code, _, out = run(board, clock, flash=False)
        self.assertEqual(code, kt.EXIT_REFUSED)
        self.assertIn("another image", out[-1])
        self.assertEqual((board.writes, board.resets), ([], 0))

    def test_busy_board_waited_for(self):
        """An access during an announced reset is tried again (the image ran without the host and resets)."""
        clock = Clock()
        board = FakeBoard(clock, ALL_PASS, ["pass", "fail", "pass"])
        board.busy = 3
        code, runs, _ = run(board, clock)
        self.assertEqual(code, kt.EXIT_FAIL)   # the run's own failure: it ran
        self.assertEqual(len(board.writes), 1)

    def test_board_away_for_good(self):
        clock = Clock()
        board = FakeBoard(clock, ALL_PASS, ["pass"] * 3)
        board.busy = 10_000
        code, _, out = run(board, clock)
        self.assertEqual(code, kt.EXIT_TROUBLE)
        self.assertIn("stayed away", out[-1])

    def test_same_image_runs(self):
        clock = Clock()
        board = FakeBoard(clock, [READY3] + ALL_PASS, ["pass", "fail", "pass"])
        code, runs, _ = run(board, clock, flash=False)
        # the run's own failure, not a refusal
        self.assertEqual(code, kt.EXIT_FAIL)
        self.assertEqual(len(runs), 1)

    def test_host_wait_from_ready(self):
        """An image that waits 20 s for the host: no reset before those 20 s are over."""
        clock = Clock()
        board = FakeBoard(clock, ALL_PASS, ["pass", "fail", "pass"], takes_request=False,
                          after_flash=[log(f"TEST|READY|n=3|seq=0|build={BUILD}|wait=20000ms")])
        at = []
        reset = board.reset

        def timed_reset():
            at.append(clock.t)
            reset()

        board.reset = timed_reset
        run(board, clock)
        self.assertEqual(len(at), 1)
        self.assertGreaterEqual(at[0], 20)

    def test_tree_build_id(self):
        """main's path: a Tree's parsed ELF (a property) -> its note's id."""
        data = note([4, 20, 3, 0x0055_4E47, 0x6789ABCD, 0x01234567])

        class Elf:
            def symbol(self, name):
                return (0x1000, 36) if name == "_LINKER_build_id_" else None

            def bytes_at(self, address, count):
                return data[:count] if address == 0x1000 else None

        tree = types.SimpleNamespace(image=types.SimpleNamespace(parsed=Elf()))
        self.assertEqual(kt.image_build_id(tree), "6789abcd01234567")

    def test_ready_without_build_id_is_no_ready(self):
        """A READY line without the build id and the wait is malformed: no READY line."""
        self.assertIsNone(kt.Results([], set()).feed("TEST|READY|n=3|seq=0"))

    def test_arm_image_note(self):
        """The note of the fixture ELF's bytes as the SDK's linker writes it (16 B header + 20 B sha1)."""
        sha1 = bytes(range(0x10, 0x24))
        raw = note([4, 20, 3, 0x0055_4E47]) + sha1
        self.assertEqual(kt.build_id_of(raw[:24]), "1312111017161514")


if __name__ == "__main__":
    unittest.main()
