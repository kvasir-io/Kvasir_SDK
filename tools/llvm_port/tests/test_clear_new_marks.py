#!/usr/bin/env python3
"""`libcport --clear-new-marks <sdk>`: strips the trailing "# NEW" a port gives each new source-list entry, in every
source list of libcport's component table (profile.cmake included), and nothing else. Needs bash, grep and sed only:
no llvm-project checkout."""
import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
LIBCPORT = HERE.parent / "libcport"
SDK = HERE.parents[2]
LIBS = ("libc", "libcxx", "compiler-rt")

# the four source lists a port syncs: (lib, file, list variable)
LISTS = (("libc", "libc.cmake", "LIBC_SOURCE_FILES"),
         ("libcxx", "libcxx.cmake", "LIBCXX_SOURCE_FILES"),
         ("compiler-rt", "compiler-rt.cmake", "COMPILER_RT_ALL_SOURCE_FILES"),
         ("compiler-rt", "profile.cmake", "COMPILER_RT_PROFILE_SOURCE_FILES"))


def clear(sdk: Path, *extra: str) -> subprocess.CompletedProcess:
    return subprocess.run(["bash", str(LIBCPORT), "--clear-new-marks", *extra, str(sdk)],
                          capture_output=True, text=True, env=os.environ | {"NO_COLOR": "1"})


class ScratchLists(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.sdk = Path(self.tmp.name)
        for lib, name, var in LISTS:
            (self.sdk / "lib" / lib).mkdir(parents=True, exist_ok=True)
            (self.sdk / "lib" / lib / name).write_text(
                "# a comment that names the `# NEW` marks mid-line, which stay\n"
                f"set({var}\n"
                f"    src/active_{var}.cpp\n"
                f"    # src/commented_{var}.cpp\n"
                f"    src/new_{var}.cpp  # NEW\n"
                ")\n")

    def tearDown(self):
        self.tmp.cleanup()

    def test_every_list_is_cleared(self):
        res = clear(self.sdk)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        self.assertIn(">> done: 4 mark(s) removed.", res.stdout)
        for lib, name, var in LISTS:
            text = (self.sdk / "lib" / lib / name).read_text()
            self.assertIsNone(
                re.search(r"# NEW\s*$", text, re.M), f"{lib}/{name}")
            self.assertIn(f"    src/active_{var}.cpp\n", text)
            self.assertIn(f"    # src/commented_{var}.cpp\n", text)
            self.assertIn(f"    src/new_{var}.cpp\n", text)
            self.assertIn(
                "names the `# NEW` marks mid-line, which stay\n", text)
            self.assertIn(
                f"   {lib}/{name}: removed 1 '# NEW' mark(s)", res.stdout)

    def test_a_component_selection_clears_only_its_lists(self):
        res = clear(self.sdk, "--libcxx")
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        self.assertIn(">> done: 1 mark(s) removed.", res.stdout)
        self.assertIn(
            "# NEW\n", (self.sdk / "lib/compiler-rt/profile.cmake").read_text())

    def test_a_file_without_marks_is_not_rewritten(self):
        clear(self.sdk)
        stamps = {p: p.stat().st_mtime_ns for p in self.sdk.glob("lib/*/*.cmake")}
        os.utime(self.sdk / "lib/libc/libc.cmake", ns=(1, 1))
        res = clear(self.sdk)
        self.assertIn(">> done: 0 mark(s) removed.", res.stdout)
        self.assertEqual(
            (self.sdk / "lib/libc/libc.cmake").stat().st_mtime_ns, 1)
        for p, t in stamps.items():
            if p.name != "libc.cmake":
                self.assertEqual(p.stat().st_mtime_ns, t, p)

    def test_an_unknown_component_is_refused(self):
        res = clear(self.sdk, "-c", "libm")
        self.assertEqual(res.returncode, 2)
        self.assertIn("unknown component 'libm'", res.stderr)

    def test_it_takes_only_the_sdk(self):
        res = subprocess.run(["bash", str(LIBCPORT), "--clear-new-marks", "/nonexistent", str(self.sdk)],
                             capture_output=True, text=True)
        self.assertEqual(res.returncode, 2)
        self.assertIn(
            "--clear-new-marks expects only <kvasir-sdk-repo>", res.stderr)


@unittest.skipUnless(all((SDK / "lib" / lib / f"{lib}.cmake").is_file() for lib in LIBS),
                     "needs the lib/{libc,libcxx,compiler-rt} submodules")
class RealLists(unittest.TestCase):
    """Every source-list file in the SDK's lib/<lib>/ is one the table names: a mark on any of their entries goes."""

    def test_a_mark_in_every_real_cmake_file_is_cleared(self):
        with tempfile.TemporaryDirectory() as tmp:
            sdk = Path(tmp)
            originals = {}
            for src in sorted(SDK.glob("lib/*/*.cmake")):
                if src.parent.name not in LIBS:
                    continue
                dst = sdk / src.relative_to(SDK)
                dst.parent.mkdir(parents=True, exist_ok=True)
                text = src.read_text()
                originals[dst] = text
                # mark every entry line, active or commented, as a port marks a new one
                marked, n = re.subn(
                    r"^(\s+(?:# )?[a-z-]+/[\w./+-]+\.(?:c|S|cpp))$", r"\1  # NEW", text, flags=re.M)
                self.assertGreater(n, 0, src)
                dst.write_text(marked)
            self.assertEqual(len(originals), 4, sorted(originals))
            res = clear(sdk)
            self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
            for dst, text in originals.items():
                self.assertEqual(dst.read_text(), text, dst)


if __name__ == "__main__":
    shutil.which("bash") or exit(77)
    unittest.main()
