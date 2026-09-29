#!/usr/bin/env python3
"""kvasir_bench.py's parts that need no board: profile ranking, chip commands."""
import contextlib
import importlib.util
import io
import shutil
import sys
import tempfile
import unittest
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


if __name__ == "__main__":
    unittest.main()
