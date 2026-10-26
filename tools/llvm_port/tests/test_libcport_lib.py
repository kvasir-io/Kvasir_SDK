#!/usr/bin/env python3
"""tools/llvm_port/libcport_lib.py, libcport's logic, on fixtures: the source-list sync (inline last entry, commented
entries, `# NEW` order, .c beside .cpp), libc_flags' quote-aware scan, the owned.txt matcher and the patch header
reader. Needs no llvm-project checkout."""
import contextlib
import io
import os
import sys
import tempfile
import unittest
import warnings
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import libcport_lib as lib  # noqa: E402


@contextlib.contextmanager
def tree(files: dict[str, str]):
    # the moved heredoc bodies open files without closing them (CPython closes them at once): not this test's subject
    warnings.simplefilter("ignore", ResourceWarning)
    with tempfile.TemporaryDirectory() as d:
        for rel, text in files.items():
            Path(d, rel).parent.mkdir(parents=True, exist_ok=True)
            Path(d, rel).write_text(text)
        yield Path(d)


class SyncSources(unittest.TestCase):
    def sync(self, cmake: str, disk: list[str], vars_=("LIST",), exts=(".cpp",)):
        with tree({**{f: "" for f in disk}, "x.cmake": cmake}) as d, contextlib.redirect_stdout(io.StringIO()) as out:
            warn = d / "warn"
            os.environ["WARN_FILE"] = str(warn)
            try:
                lib.sync_sources(str(d / "x.cmake"), str(d),
                                 list(vars_), "src", list(exts))
            finally:
                del os.environ["WARN_FILE"]
            return (d / "x.cmake").read_text(), out.getvalue(), warn.read_text() if warn.exists() else ""

    def test_new_files_go_in_commented_and_in_order(self):
        text, out, warn = self.sync("set(LIST\n    src/b.cpp\n    src/d.cpp\n)\n", ["src/b.cpp", "src/c.cpp",
                                                                                    "src/d.cpp", "src/a.cpp"])
        self.assertEqual(text, "set(LIST\n    # src/a.cpp  # NEW\n    src/b.cpp\n    # src/c.cpp  # NEW\n"
                               "    src/d.cpp\n)\n")
        self.assertIn("+2 new, -0 removed", out)
        self.assertIn("2 new source(s) added", warn)

    def test_an_inline_last_entry_and_a_commented_one_are_kept(self):
        text, _, _ = self.sync(
            "set(LIST\n    # src/a.cpp\n    src/b.cpp)\n", ["src/a.cpp", "src/b.cpp"])
        self.assertEqual(text, "set(LIST\n    # src/a.cpp\n    src/b.cpp\n)\n")

    def test_a_gone_active_entry_is_a_warning_a_gone_commented_one_is_not(self):
        text, out, warn = self.sync(
            "set(LIST\n    src/a.cpp\n    # src/b.cpp\n    src/c.cpp\n)\n", ["src/c.cpp"])
        self.assertEqual(text, "set(LIST\n    src/c.cpp\n)\n")
        self.assertIn("-2 removed (1 of them were active)", out)
        self.assertIn("active entry gone: src/a.cpp", out)
        self.assertIn("WARN\tx.cmake: 1 ACTIVE source(s) gone", warn)

    def test_a_c_file_is_not_its_cpp_neighbour(self):
        text, _, _ = self.sync(
            "set(LIST\n    src/a.cpp\n)\n", ["src/a.c", "src/a.cpp"], exts=(".c", ".cpp"))
        self.assertIn("# src/a.c  # NEW", text)
        self.assertEqual(text.count("src/a.cpp"), 1)

    def test_every_var_counts_as_listed_the_first_gets_the_new_ones(self):
        text, _, _ = self.sync("set(LIST\n    src/a.cpp\n)\nset(MORE\n    src/b.cpp\n)\n",
                               ["src/a.cpp", "src/b.cpp", "src/c.cpp"], vars_=("LIST", "MORE"))
        self.assertEqual(
            text, "set(LIST\n    src/a.cpp\n    # src/c.cpp  # NEW\n)\nset(MORE\n    src/b.cpp\n)\n")


class CheckFlags(unittest.TestCase):
    CONFIG = '{"errno": {"LIBC_CONF_ERRNO_MODE": {"value": "LIBC_ERRNO_MODE_SHARED"}}, ' \
             '"math": {"LIBC_CONF_MATH_OPTIMIZATIONS": {"value": "(LIBC_MATH_SKIP_ACCURATE_PASS | LIBC_MATH_SMALL_TABLES)"}}}'

    def check(self, flags: str) -> str:
        with tree({"config.json": self.CONFIG, "libc.cmake": flags}) as d, \
                contextlib.redirect_stdout(io.StringIO()) as out:
            lib.check_flags(str(d / "config.json"), str(d / "libc.cmake"))
        return out.getvalue()

    def test_parentheses_inside_quotes_do_not_end_the_list(self):
        out = self.check('set(libc_flags\n  -DLIBC_ERRNO_MODE=LIBC_ERRNO_MODE_SHARED  # a comment (with parens)\n'
                         '  "-DLIBC_MATH=(LIBC_MATH_SKIP_ACCURATE_PASS|LIBC_MATH_SMALL_TABLES)")\n'
                         'set(other -DLIBC_ERRNO_MODE=WRONG)\n')
        self.assertIn("OK   LIBC_ERRNO_MODE = LIBC_ERRNO_MODE_SHARED", out)
        self.assertIn("OK   LIBC_MATH", out)
        self.assertIn("in sync", out)

    def test_a_drift_is_named(self):
        out = self.check(
            'set(libc_flags\n  -DLIBC_ERRNO_MODE=LIBC_ERRNO_MODE_THREAD_LOCAL)\n')
        self.assertIn(
            "WARN LIBC_ERRNO_MODE: config.json wants 'LIBC_ERRNO_MODE_SHARED'", out)


class OwnedMatcher(unittest.TestCase):
    def owned(self, text: str) -> lib.Owned:
        with tree({"owned.txt": text}) as d:
            return lib.Owned(str(d / "owned.txt"))

    def test_literal_paths_directories_comments(self):
        o = self.owned("# a comment\n\ninclude/stddef.h\n  kvasir/\n")
        self.assertEqual(o.entries, ["include/stddef.h", "kvasir/"])
        self.assertTrue(o.is_owned("include/stddef.h"))
        self.assertFalse(o.is_owned("include/stddef.hpp"))
        self.assertTrue(o.is_owned("kvasir/arm/memory_v6m.S"))
        self.assertFalse(o.is_owned("kvasir"),
                         "a directory entry owns what is under it")
        self.assertFalse(o.is_owned("src/kvasir/x.c"))

    def test_globs_mean_what_rsync_makes_of_them(self):
        o = self.owned("include/*.h\nsrc/**/k_*.c\n")
        self.assertTrue(o.is_owned("include/a.h"))
        self.assertFalse(o.is_owned("include/sys/a.h"),
                         "* stays within one component")
        self.assertTrue(o.is_owned("src/a/b/k_x.c"), "** crosses them")
        self.assertFalse(o.is_owned("src/a/b/x.c"))

    def test_the_mirror_filter_of_one_top_level_dir(self):
        o = self.owned("include/stddef.h\ninclude/__gcc-compat/\nsrc/x.cpp\n")
        self.assertEqual(o.rsync_filter("include"), [
                         "- /stddef.h", "- /__gcc-compat/"])
        self.assertEqual(o.rsync_filter("lib"), [])

    def test_dead_entries(self):
        with tree({"owned.txt": "a/b.c\nd/\nmissing.h\n", "dest/a/b.c": "", "dest/d/e": ""}) as d:
            self.assertEqual(lib.Owned(str(d / "owned.txt")
                                       ).dead(str(d / "dest")), ["missing.h"])

    def test_no_file_no_entries(self):
        self.assertEqual(lib.Owned("/nonexistent/owned.txt").entries, [])


class PatchHeader(unittest.TestCase):
    GOOD = ("Kvasir patch: libc src/x.cpp\nWhy:       it breaks\nUpstream:  none - ours\nProof:     test\n"
            "Drop when: never\n\n--- a/src/x.cpp\n+++ b/src/x.cpp\n")

    def test_the_block_before_the_diff(self):
        self.assertEqual(lib.patch_header(self.GOOD),
                         self.GOOD.split("\n--- a/")[0])
        self.assertEqual(lib.patch_header("--- a/x\n+++ b/x\n"), "")

    def test_complete_missing_and_todo(self):
        self.assertIsNone(lib.header_problem(self.GOOD))
        self.assertEqual(lib.header_problem("--- a/x\n+++ b/x\n"), "")
        self.assertEqual(lib.header_problem(
            self.GOOD.replace("Proof:     test\n", "")), "")
        self.assertEqual(lib.header_problem(self.GOOD.replace(
            "never", "TODO")), " (a TODO left in its header)")

    def test_check_lines_and_carried_headers(self):
        with tree({"p/a.patch": self.GOOD, "p/sub/b.patch": "--- a/b\n+++ b/b\n",
                   "p/c.patch": self.GOOD.replace("never", "TODO")}) as d, \
                contextlib.redirect_stdout(io.StringIO()) as out:
            lib.patch_check(str(d / "p"))
            lib.patch_headers(str(d / "p"), str(d / "h.json"))
            import json
            headers = json.loads((d / "h.json").read_text())
        self.assertEqual(out.getvalue().splitlines(), ["   NOHEADER c.patch (a TODO left in its header)",
                                                       "   NOHEADER sub/b.patch"])
        self.assertEqual(sorted(headers), ["a.patch", "c.patch"])
        self.assertTrue(headers["a.patch"].endswith("Drop when: never\n\n"))


if __name__ == "__main__":
    unittest.main()
