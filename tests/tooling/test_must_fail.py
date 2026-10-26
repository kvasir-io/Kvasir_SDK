#!/usr/bin/env python3
"""tests/tooling/must_fail.py's own rules, on a fake compiler: a missing want fails, a must-fail case that compiles
fails, a control that does not compile fails, a tuple's order is checked, clang_only is wanted only from clang, the
configuration's flags reach the compiler, a failing controls_first stops the run, the report keeps the list's order
while the cases compile in parallel.

    test_must_fail.py
"""
import contextlib
import io
import stat
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import must_fail  # noqa: E402
from must_fail import Case  # noqa: E402

# The fake compiler: `--version` prints its name; otherwise it reads the source (the last argument) and obeys its
# lines `EXIT <n>`, `SAY <text>`, `SLEEP <s>`, `ECHOFLAGS` (prints its argv).
FAKE = textwrap.dedent('''\
    #!{python}
    import sys, time
    if sys.argv[1:] == ["--version"]:
        print("{name} version 99"); sys.exit(0)
    rc = 0
    for line in open(sys.argv[-1]).read().splitlines():
        if line.startswith("EXIT "): rc = int(line[5:])
        elif line.startswith("SAY "): print(line[4:], file=sys.stderr)
        elif line.startswith("SLEEP "): time.sleep(float(line[6:]))
        elif line == "ECHOFLAGS": print(" ".join(sys.argv[1:-1]))
    sys.exit(rc)
''')


class MustFailRunner(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.cxx = {}
        for name in ("clang", "gcc"):
            p = Path(cls.tmp.name) / f"fake-{name}"
            p.write_text(FAKE.format(python=sys.executable, name=name))
            p.chmod(p.stat().st_mode | stat.S_IXUSR)
            cls.cxx[name] = str(p)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def go(self, cases, compiler="gcc", extra=(), **kw):
        out = io.StringIO()
        argv = ["--cxx", self.cxx[compiler], "--src",
                "/the/src", "--tests", "/the/tests", *extra]
        with contextlib.redirect_stdout(out):
            rc = must_fail.run("", cases, argv=argv, **kw)
        return rc, out.getvalue()

    def test_a_failing_case_with_its_text_holds(self):
        rc, out = self.go(
            [Case("c", "EXIT 1\nSAY the reason\n", ["the reason"])])
        self.assertEqual(rc, 0, out)
        self.assertIn("ok   c", out)

    def test_a_missing_want_fails(self):
        rc, out = self.go([Case("c", "EXIT 1\nSAY other\n", ["the reason"])])
        self.assertEqual(rc, 1)
        self.assertIn("FAIL c: exit 1, missing ['the reason']", out)

    def test_a_must_fail_case_that_compiles_fails(self):
        rc, out = self.go([Case("c", "SAY the reason\n", ["the reason"])])
        self.assertEqual(rc, 1)
        self.assertIn("FAIL c: exit 0", out)

    def test_a_control_must_compile(self):
        self.assertEqual(
            self.go([Case("c", "", None), Case("d", "", [])])[0], 0)
        rc, out = self.go([Case("c", "EXIT 1\n")])
        self.assertEqual(rc, 1)
        self.assertIn("FAIL c: the control does not compile", out)

    def test_a_tuple_is_wanted_in_order(self):
        body = "EXIT 1\nSAY first\nSAY second\n"
        self.assertEqual(self.go([Case("c", body, ("first", "second"))])[0], 0)
        self.assertEqual(self.go([Case("c", body, ["second", "first"])])[0], 0)
        rc, out = self.go([Case("c", body, ("second", "first"))])
        self.assertEqual(rc, 1)
        self.assertIn("missing ['first']", out)

    def test_clang_only_is_wanted_only_from_clang(self):
        case = Case("c", "EXIT 1\nSAY both\n", [
                    "both"], clang_only=["clang says"])
        self.assertEqual(self.go([case], "gcc")[0], 0)
        self.assertEqual(self.go([case], "clang")[0], 1)
        ok = Case("c", "EXIT 1\nSAY both\nSAY clang says\n",
                  ["both"], clang_only=["clang says"])
        self.assertEqual(self.go([ok], "clang")[0], 0)

    def test_the_flags_reach_the_compiler(self):
        # the configuration's flags first, then the suite's, then the case's (with @TESTS@ replaced)
        case = Case("c", "EXIT 1\nECHOFLAGS\n", ("-std=c++26", "-stdlib=libc++", "-DSUITE", "-fsyntax-only",
                                                 "-I/the/src", "-I/the/tests", "-I/the/tests/support"),
                    flags=["-I@TESTS@/support"])
        rc, out = self.go([case], extra=["--flag=-std=c++26",
                          "--flag=-stdlib=libc++"], flags=["-DSUITE"])
        self.assertEqual(rc, 0, out)

    def test_a_case_head_replaces_the_suite_head(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rc = must_fail.run("EXIT 0\n", [Case("c", "SAY x\n", ["x"], head="EXIT 1\n")],
                               argv=["--cxx", self.cxx["gcc"], "--src", "/s"])
        self.assertEqual(rc, 0, out.getvalue())

    def test_a_failing_first_control_stops_the_run(self):
        rc, out = self.go([Case("never", "EXIT 1\n", ["x"])],
                          controls_first=[Case("control", "EXIT 1\n")])
        self.assertEqual(rc, 1)
        self.assertIn("FAIL control", out)
        self.assertNotIn("never", out)

    def test_the_report_keeps_the_list_order(self):
        # the first case finishes last
        cases = [Case(f"c{i}", f"EXIT 1\nSLEEP {0.4 - i * 0.1}\nSAY w\n", ["w"])
                 for i in range(4)]
        rc, out = self.go(cases, extra=["--jobs", "4"])
        self.assertEqual(rc, 0, out)
        lines = [ln for ln in out.splitlines() if ln.startswith("ok")]
        self.assertEqual(lines, [f"ok   c{i}" for i in range(4)])


if __name__ == "__main__":
    unittest.main()
