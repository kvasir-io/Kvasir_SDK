#!/usr/bin/env python3
"""Tests of tools/header_cost.py on a fixture tree written into a temporary directory: a fake compile_commands.json
whose one TU includes a plain header and a generated one, compiled by the host's clang++ (or c++; none: a skip).
Lines, files, the generated share, the include roots, the ceiling check's three outcomes and its refusals. The
instruction count (perf, machine-dependent) is not tested."""

import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.join(os.path.dirname(
    os.path.abspath(__file__)), "..", "..", "tools"))
import header_cost  # noqa: E402

CXX = shutil.which("clang++") or shutil.which("c++")

PLAIN = "#pragma once\nint plainA();\n\nint plainB();\n"
GENERATED = "#pragma once\nnamespace gen {\nint r0();\nint r1();\n}\n"
MAIN = '#include "plain.hpp"\n#include "peripherals/X.hpp"\n#include "plain.hpp"\n\nint main() { return 0; }\n'


@unittest.skipUnless(CXX, "no host C++ compiler")
class HeaderCost(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="header_cost_")
        self.tree = os.path.join(self.dir, "build")
        os.makedirs(os.path.join(self.dir, "inc"))
        os.makedirs(os.path.join(
            self.tree, "svd_converter_generated", "peripherals"))
        self.write("inc/plain.hpp", PLAIN)
        self.write("build/svd_converter_generated/peripherals/X.hpp", GENERATED)
        self.write("src/main.cpp", MAIN)
        self.write("src/other.cpp", "int other() { return 1; }\n")
        gen = os.path.join(self.tree, "svd_converter_generated")
        entries = [
            {"directory": self.tree, "file": os.path.join(self.dir, "src", name),
             "command": f"{CXX} -std=c++26 -I{os.path.join(self.dir, 'inc')} -isystem {gen} -flto=thin -MD -MT x "
             f"-MF CMakeFiles/{target}.dir/src/{name}.o.d -o CMakeFiles/{target}.dir/src/{name}.o "
             f"-c {os.path.join(self.dir, 'src', name)}"}
            for target, name in (("app_release", "main.cpp"), ("app_release", "other.cpp"), ("app_debug", "main.cpp"))]
        self.write("build/compile_commands.json", json.dumps(entries))

    def tearDown(self):
        shutil.rmtree(self.dir)

    def write(self, rel, text):
        path = os.path.join(self.dir, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as f:
            f.write(text)

    def run_main(self, *argv):
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            rc = header_cost.main(list(argv))
        return rc, out.getvalue(), err.getvalue()

    def test_measure_counts(self):
        r = header_cost.measure(self.tree, "app_release",
                                "main.cpp", "svd_converter_generated")
        # plain.hpp 2 lines once (the pragma is no output line), X.hpp 4, main.cpp 1
        self.assertEqual(r["lines"], 2 + 4 + 1)
        self.assertEqual(r["files"], 3)
        self.assertEqual((r["generated_files"], r["generated_lines"]), (1, 4))
        self.assertEqual(r["by_root"][os.path.join(self.dir, "inc")], 2)
        self.assertEqual(r["by_root"][os.path.join(
            self.tree, "svd_converter_generated")], 4)
        self.assertEqual(r["by_root"]["(source and other)"], 1)
        self.assertNotIn("instructions", r)

    def test_lines_are_what_e_p_keeps(self):
        entry = header_cost.load_entry(self.tree, "app_release", "main.cpp")
        run = subprocess.run(header_cost.base_args(entry) + ["-E", "-P", "-o", "-"], cwd=self.tree,
                             capture_output=True, text=True, check=True)
        oracle = sum(1 for line in run.stdout.splitlines() if line.strip())
        self.assertEqual(header_cost.measure(
            self.tree, "app_release", "main.cpp", "x")["lines"], oracle)

    def test_measure_picks_the_target(self):
        r = header_cost.measure(self.tree, "app_release",
                                "other.cpp", "svd_converter_generated")
        self.assertEqual(
            (r["lines"], r["files"], r["generated_lines"]), (1, 1, 0))

    def test_measure_cli_and_json(self):
        rc, out, _ = self.run_main("measure", self.tree, "app_debug")
        self.assertEqual(rc, 0)
        self.assertIn(
            "app_debug main.cpp: 7 lines, 3 files, generated 1 files / 4 lines (57.1 %)", out)
        rc, out, _ = self.run_main(
            "measure", self.tree, "app_release:main.cpp", "app_release:other.cpp", "--json")
        self.assertEqual(rc, 0)
        self.assertEqual([r["lines"] for r in json.loads(out)], [7, 1])
        # the shared options after the subcommand, as the usage writes them
        rc, out, _ = self.run_main(
            "measure", self.tree, "app_debug", "--generated", "nowhere", "--top", "0")
        self.assertEqual(rc, 0)
        self.assertIn("generated 0 files / 0 lines", out)

    def test_measure_refusals(self):
        rc, _, err = self.run_main("measure", self.tree, "nope")
        self.assertEqual(rc, 2)
        self.assertIn("no compile command of target 'nope'", err)
        rc, _, err = self.run_main("measure", self.tree, "app_release:.cpp")
        self.assertEqual(rc, 2)
        self.assertIn("names several files", err)
        rc, _, err = self.run_main("measure", os.path.join(
            self.dir, "missing"), "app_release")
        self.assertEqual(rc, 2)
        self.write("inc/plain.hpp", "#error broken\n")
        rc, _, err = self.run_main("measure", self.tree, "app_release")
        self.assertEqual(rc, 2)
        self.assertIn("preprocessing failed", err)

    def ceilings(self, text):
        self.write("ceilings.toml", text)
        return os.path.join(self.dir, "ceilings.toml")

    def test_check_outcomes(self):
        tu = '[[tu]]\ntree = "build"\ntarget = "app_release"\nfile = "main.cpp"\nlines = {}\n'
        rc, out, _ = self.run_main("check", self.ceilings(
            "margin_percent = 2\n" + tu.format(7)))
        self.assertEqual(rc, 0)
        self.assertIn(
            "ok       build app_release main.cpp: 7 lines, ceiling 7 + 2 %", out)
        rc, out, _ = self.run_main("check", self.ceilings(
            "margin_percent = 10\n" + tu.format(6)))
        self.assertEqual(rc, 1)
        self.assertIn(
            "OVER     build app_release main.cpp: 7 lines > ceiling 6 + 10 %", out)
        rc, out, _ = self.run_main("check", self.ceilings(
            "margin_percent = 20\n" + tu.format(6)))
        self.assertEqual(rc, 0, "7 <= 6 * 1.2: inside the margin")
        rc, out, _ = self.run_main("check", self.ceilings(
            "margin_percent = 2\n" + tu.format(20)))
        self.assertEqual(rc, 0)
        self.assertIn(
            "BELOW    build app_release main.cpp: 7 lines, ceiling 20 (lower it)", out)

    def test_check_tree_override(self):
        tu = '[[tu]]\ntree = "elsewhere"\ntarget = "app_release"\nfile = "main.cpp"\nlines = 7\n'
        path = self.ceilings("margin_percent = 2\n" + tu)
        rc, _, err = self.run_main("check", path)
        self.assertEqual(rc, 2, "the file's own tree does not exist")
        self.assertIn("elsewhere", err)
        rc, out, _ = self.run_main("check", path, "--tree", self.tree)
        self.assertEqual(rc, 0)
        self.assertIn(
            f"ok       {self.tree} app_release main.cpp: 7 lines, ceiling 7 + 2 %", out)

    def test_check_refusals(self):
        rc, _, err = self.run_main(
            "check", self.ceilings("margin_percent = 2\n"))
        self.assertEqual(rc, 2)
        self.assertIn("needs [[tu]] tables", err)
        rc, _, err = self.run_main("check", self.ceilings('[[tu]]\ntree = "build"\ntarget = "app_release"\n'
                                                          'lines = 8\nceiling = 3\n'))
        self.assertEqual(rc, 2)
        self.assertIn("unknown: ceiling", err)
        rc, _, err = self.run_main("check", self.ceilings("[[tu]\n"))
        self.assertEqual(rc, 2)


if __name__ == "__main__":
    unittest.main()
