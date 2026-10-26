#!/usr/bin/env python3
"""Tests of fw.py: the guard, the build-all filter (against the bash filter it replaced, as an oracle) and the
stale-tree scan. A fake `cmake` on PATH prints a fixture and exits with a given code; nothing outside a temporary
directory is read or written."""

import io
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stdout

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fw  # noqa: E402

FW = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fw.py")

# The firmware justfiles' build-all body before fw.py, verbatim but for the target list and the message.
ORACLE = r'''
set -uo pipefail
# A compiler diagnostic, not the mere word: object paths such as
# boost_process.dir/src/error.cpp.o used to trip a bare /warning|error/ and
# fail a build that was in fact clean.
diag=': (warning|error|fatal error):|^FAILED:|^ninja: (error|build stopped)'
out=$(cmake --build "/build" --target debug release 2>&1)
rc=$?
if [ $rc -ne 0 ]; then
    # Every failed edge whole: ninja prints a command's output after its FAILED line
    # and the command, and what a post-link step or a script says (check_ram_funcs, a
    # traceback) matches no diagnostic pattern. The output's tail if nothing FAILED.
    failed=$(awk '/^FAILED:/ { on = 1; n = 0; print substr($0, 1, 200); next }
                  /^\[[0-9]+\/[0-9]+\] |^ninja: / { on = 0 }
                  on && ++n <= 40 { print substr($0, 1, 400) }
                  /^ninja: (error|build stopped)/ { print }' <<<"$out")
    if [ -n "$failed" ]; then echo "$failed"; else tail -n 30 <<<"$out"; fi
    echo "BUILD FAILED" >&2
    exit $rc
fi
if grep -nE "$diag" <<<"$out"; then
    echo "NOT CLEAN - warnings above" >&2
    exit 1
fi
echo "all variants built warning-free"
'''

FAKE_CMAKE = '#!/bin/sh\ncat "$FW_FIXTURE"\nexit "$FW_RC"\n'


def progress(i, n=3):
    return f"[{i}/{n}] Building CXX object CMakeFiles/app.dir/src/main.cpp.o"


def compile_failure(lines):
    return ([progress(1), "FAILED: CMakeFiles/app.dir/src/main.cpp.o",
             "/usr/bin/clang++ -c /src/main.cpp -o CMakeFiles/app.dir/src/main.cpp.o"]
            + [f"/src/main.cpp:{i}:1: note: line {i} of the output" for i in range(lines)]
            + ["ninja: build stopped: subcommand failed."])


FIXTURES = {
    "clean": ([progress(1), progress(2), progress(3)], 0),
    "empty": ([], 0),
    "empty_failure": ([], 2),
    "warning": ([progress(1), "/src/main.cpp:3:5: warning: unused variable 'x' [-Wunused-variable]",
                 "    3 |     int x;", "      |         ^", progress(2)], 0),
    "error_word_in_path": ([progress(1), "[2/3] Building CXX object boost_process.dir/src/error.cpp.o",
                            "[3/3] Linking warning_free.elf"], 0),
    "compile_failure_60": (compile_failure(60), 1),
    "postlink_traceback": ([progress(1), "FAILED: app.elf",
                            "cd /build && python3 -B /sdk/cmake/tools/kvasir_postlink.py app.elf",
                            "Traceback (most recent call last):", '  File "/sdk/tools/x.py", line 3, in <module>',
                            "    main()", "ValueError: section .data ends past the flash image end",
                            "ninja: build stopped: subcommand failed."], 1),
    "two_failures": ([progress(1), "FAILED: a.o", "a.cpp:1:1: error: boom", progress(2), "FAILED: b.o",
                      "b.cpp:1:1: error: bang", "ninja: build stopped: subcommand failed."], 1),
    "ninja_error": (["ninja: error: unknown target 'no_such_target'"], 1),
    "failure_without_failed": ([f"configure line {i}" for i in range(50)], 1),
    "long_lines": ([progress(1), "FAILED: " + "x" * 300, "y" * 500 + "TAIL",
                    "ninja: build stopped: subcommand failed."], 1),
    "non_ascii": ([progress(1), "FAILED: ü.o", "ä" * 450, "/src/ö.cpp:1:1: warning: größe",
                   "ninja: build stopped: subcommand failed."], 1),
    "non_ascii_warning": (["/src/ö.cpp:1:1: warning: größe [-Wx]", "ü" * 10], 0),
    "trailing_blank_lines": ([progress(1), "/src/a.cpp:1:1: warning: w", "", "", ""], 0),
    "failed_block_blank_end": ([progress(1), "FAILED: a.o", "a.cpp:1:1: error: e", "", ""], 1),
    "fatal_error": (["/src/a.cpp:1:10: fatal error: 'x.h' file not found"], 0),
    "carriage_return": (["[1/2] a\r[2/2] b", "/src/a.cpp:1:1: warning: cr\rmore"], 0),
    "signal": ([progress(1), "FAILED: a.o"], 137),
}


def write_fixture(path, lines, trailing="\n"):
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + (trailing if lines else ""))


class FakeCmake:
    """A temporary directory with a fake cmake first on PATH."""

    def __enter__(self):
        self.dir = tempfile.mkdtemp(prefix="fw_test_")
        cmake = os.path.join(self.dir, "cmake")
        with open(cmake, "w") as f:
            f.write(FAKE_CMAKE)
        os.chmod(cmake, os.stat(cmake).st_mode | stat.S_IXUSR)
        self.fixture = os.path.join(self.dir, "fixture.txt")
        self.env = dict(os.environ, PATH=self.dir + os.pathsep + os.environ.get("PATH", ""),
                        FW_FIXTURE=self.fixture, LC_ALL="C.UTF-8")
        return self

    def __exit__(self, *exc):
        shutil.rmtree(self.dir, ignore_errors=True)

    def run(self, cmd):
        return subprocess.run(cmd, env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    def fw(self, rc):
        self.env["FW_RC"] = str(rc)
        return self.run([sys.executable, "-B", FW, "build-all", "--tree", "/build",
                         "--ok", "all variants built warning-free", "--", "debug", "release"])

    def oracle(self, rc):
        self.env["FW_RC"] = str(rc)
        return self.run(["bash", "-c", ORACLE])


def oracle_missing():
    return shutil.which("bash") is None or shutil.which("awk") is None or shutil.which("grep") is None


class BuildAll(unittest.TestCase):
    def check(self, name, lines, rc):
        with FakeCmake() as fake:
            write_fixture(fake.fixture, lines)
            new = fake.fw(rc)
            if oracle_missing():
                self.skipTest("bash, awk or grep missing: no oracle")
            old = fake.oracle(rc)
            self.assertEqual(new.stdout, old.stdout, f"{name}: stdout")
            self.assertEqual(new.stderr, old.stderr, f"{name}: stderr")
            self.assertEqual(new.returncode, old.returncode, f"{name}: rc")
            return new

    def test_oracle_every_fixture(self):
        for name, (lines, rc) in FIXTURES.items():
            with self.subTest(name):
                self.check(name, lines, rc)

    def test_clean_prints_the_message_only(self):
        r = self.check("clean", *FIXTURES["clean"])
        self.assertEqual(r.stdout, b"all variants built warning-free\n")
        self.assertEqual(r.stderr, b"")
        self.assertEqual(r.returncode, 0)

    def test_warning_is_not_clean(self):
        r = self.check("warning", *FIXTURES["warning"])
        self.assertEqual(r.returncode, 1)
        self.assertIn(b"2:/src/main.cpp:3:5: warning:", r.stdout)
        self.assertEqual(r.stderr, b"NOT CLEAN - warnings above\n")

    def test_error_in_an_object_path_stays_clean(self):
        r = self.check("error_word_in_path", *FIXTURES["error_word_in_path"])
        self.assertEqual(r.returncode, 0)

    def test_failed_edge_cut_at_40_lines(self):
        r = self.check("compile_failure_60", *FIXTURES["compile_failure_60"])
        out = r.stdout.decode().split("\n")
        self.assertEqual(out[0], "FAILED: CMakeFiles/app.dir/src/main.cpp.o")
        self.assertEqual(
            len([line for line in out if "of the output" in line]), 39)
        self.assertEqual(out[-2], "ninja: build stopped: subcommand failed.")
        self.assertEqual(r.stderr, b"BUILD FAILED\n")
        self.assertEqual(r.returncode, 1)

    def test_traceback_shown(self):
        r = self.check("postlink_traceback", *FIXTURES["postlink_traceback"])
        self.assertIn(b"ValueError: section .data", r.stdout)

    def test_tail_without_failed(self):
        r = self.check("failure_without_failed", *
                       FIXTURES["failure_without_failed"])
        out = r.stdout.decode().rstrip("\n").split("\n")
        self.assertEqual(out, [f"configure line {i}" for i in range(20, 50)])

    def test_signal_exit_code(self):
        r = self.check("signal", *FIXTURES["signal"])
        self.assertEqual(r.returncode, 137)

    def test_no_targets_builds_the_tree(self):
        with FakeCmake() as fake:
            with open(os.path.join(fake.dir, "cmake"), "w") as f:
                f.write('#!/bin/sh\necho "$@"\n')
            fake.env["FW_RC"] = "0"
            r = fake.run([sys.executable, "-B", FW, "build-all",
                         "--tree", "/b", "--ok", "ok"])
            self.assertEqual(r.stdout, b"ok\n")
            r = fake.run([sys.executable, "-B", FW, "build-all",
                         "--tree", "/b", "--ok", "ok", "--", "t1", "t2"])
            self.assertEqual(r.stdout, b"ok\n")

    def test_arguments_reach_cmake(self):
        with FakeCmake() as fake:
            log = os.path.join(fake.dir, "argv")
            with open(os.path.join(fake.dir, "cmake"), "w") as f:
                f.write(f'#!/bin/sh\nprintf "%s|" "$@" > "{log}"\n')
            for extra, want in (([], "--build|/b|"), (["--", "t1", "t2"], "--build|/b|--target|t1|t2|"),
                                (["t1"], "--build|/b|--target|t1|")):
                fake.run([sys.executable, "-B", FW, "build-all",
                         "--tree", "/b", "--ok", "ok", *extra])
                with open(log) as f:
                    self.assertEqual(f.read(), want)

    def test_filter_in_process(self):
        self.assertEqual(fw.filter_build("", 0, "ok"), ("ok\n", "", 0))
        self.assertEqual(fw.filter_build("", 3, "ok"),
                         ("\n", "BUILD FAILED\n", 3))
        self.assertEqual(fw.filter_build("a: warning: b", 0, "ok"),
                         ("1:a: warning: b\n", "NOT CLEAN - warnings above\n", 1))


def cache(**entries):
    """A CMakeCache.txt text from NAME=(type, value) or NAME=value (INTERNAL)."""
    out = ["# This is the CMakeCache file.", ""]
    for name, value in entries.items():
        if value is None:
            continue
        kind, value = value if isinstance(
            value, tuple) else ("INTERNAL", value)
        out.append(f"{name}:{kind}={value}")
    return "\n".join(out) + "\n"


class Guard(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="fw_guard_")
        self.src = os.path.join(self.tmp, "src")
        self.dep = os.path.join(self.tmp, "dep")
        os.makedirs(self.src)
        os.makedirs(self.dep)
        self.tree = os.path.join(self.tmp, "build")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def good(self, **extra):
        entries = dict(CMAKE_HOME_DIRECTORY=self.src, CMAKE_GENERATOR="Ninja", CMAKE_PROJECT_NAME=("STATIC", "app"),
                       CMAKE_ROOT="/usr/share/cmake", KVASIR_ROOT=self.dep, CHIP_ROOT=self.dep)
        entries.update(extra)
        return {k: v for k, v in entries.items() if v is not None}

    def run_guard(self, text, *expect):
        if text is not None:
            os.makedirs(self.tree, exist_ok=True)
            with open(os.path.join(self.tree, "CMakeCache.txt"), "w") as f:
                f.write(text)
        args = ["guard", "--tree", self.tree, "--source", self.src]
        for e in expect:
            args += ["--expect", e]
        buf = io.StringIO()
        with redirect_stdout(buf):
            rc = fw.main(args)
        self.assertEqual(rc, 0)
        return buf.getvalue()

    def assert_wiped(self, out, why):
        self.assertEqual(out, f"==> wiping {self.tree}: {why}\n")
        self.assertFalse(os.path.exists(self.tree))

    def assert_kept(self, out):
        self.assertEqual(out, "")
        self.assertTrue(os.path.isfile(
            os.path.join(self.tree, "CMakeCache.txt")))

    def test_no_cache(self):
        self.assertEqual(self.run_guard(None), "")
        os.makedirs(self.tree)
        self.assertEqual(self.run_guard(None), "")
        self.assertTrue(os.path.isdir(self.tree))

    def test_clean_cache_kept(self):
        self.assert_kept(self.run_guard(cache(**self.good())))

    def test_source_moved(self):
        self.assert_wiped(self.run_guard(cache(**self.good(CMAKE_HOME_DIRECTORY="/old/src"))),
                          "source moved (/old/src)")

    def test_source_missing_entry(self):
        self.assert_wiped(self.run_guard(
            cache(**self.good(CMAKE_HOME_DIRECTORY=None))), "source moved ()")

    def test_root_gone_any_name(self):
        out = self.run_guard(
            cache(**self.good(SLOOK_ROOT=("UNINITIALIZED", "/gone/slook"))))
        self.assert_wiped(out, "SLOOK_ROOT gone (/gone/slook)")

    def test_cmake_root_and_empty_roots_ignored(self):
        self.assert_kept(self.run_guard(
            cache(**self.good(CMAKE_ROOT="/gone/cmake", GFX_ROOT=("PATH", "")))))

    def test_first_entry_of_a_name_counts(self):
        text = cache(**self.good()) + "KVASIR_ROOT:PATH=/gone/second\n"
        self.assert_kept(self.run_guard(text))

    def test_never_finished(self):
        self.assert_wiped(self.run_guard(
            cache(**self.good(CMAKE_PROJECT_NAME=None))), "configure never finished")

    def test_generator(self):
        out = self.run_guard(
            cache(**self.good(CMAKE_GENERATOR="Unix Makefiles")))
        self.assert_wiped(out, "generator is 'Unix Makefiles', not Ninja")

    def test_expect(self):
        other = os.path.join(self.tmp, "other")
        self.assert_kept(self.run_guard(
            cache(**self.good()), f"CHIP_ROOT={self.dep}"))
        self.assert_wiped(self.run_guard(cache(**self.good()), f"CHIP_ROOT={other}"),
                          f"CHIP_ROOT is {self.dep}, not {other}")

    def test_last_reason_wins(self):
        text = cache(**self.good(CMAKE_HOME_DIRECTORY="/old",
                     KVASIR_ROOT="/gone", CMAKE_PROJECT_NAME=None))
        self.assert_wiped(self.run_guard(text), "configure never finished")
        text = cache(**self.good(CMAKE_HOME_DIRECTORY="/old",
                     KVASIR_ROOT="/gone", CMAKE_GENERATOR="Xcode"))
        self.assert_wiped(self.run_guard(
            text), "generator is 'Xcode', not Ninja")
        text = cache(
            **self.good(CMAKE_HOME_DIRECTORY="/old", KVASIR_ROOT="/gone"))
        self.assert_wiped(self.run_guard(text), "KVASIR_ROOT gone (/gone)")
        other = os.path.join(self.tmp, "other")
        self.assert_wiped(self.run_guard(cache(**self.good(KVASIR_ROOT="/gone")), f"CHIP_ROOT={other}"),
                          f"CHIP_ROOT is {self.dep}, not {other}")

    def test_bad_expect(self):
        with self.assertRaises(SystemExit):
            fw.main(["guard", "--tree", self.tree, "--source",
                    self.src, "--expect", "CHIP_ROOT"])


class StaleTrees(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="fw_stale_")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def tree(self, rel, text):
        d = os.path.join(self.tmp, rel)
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, "CMakeCache.txt"), "w") as f:
            f.write(text)
        return d

    def test_scan(self):
        src = os.path.join(self.tmp, "repo")
        os.makedirs(src)
        ok = dict(CMAKE_HOME_DIRECTORY=src, CMAKE_GENERATOR="Ninja",
                  CMAKE_PROJECT_NAME=("STATIC", "x"))
        self.tree("repo/build", cache(**ok))
        self.tree("repo/build/nested",
                  cache(**dict(ok, CMAKE_HOME_DIRECTORY="/gone")))
        self.tree("repo/build/_deps/x-build",
                  cache(**dict(ok, CMAKE_GENERATOR="Xcode")))
        self.tree("repo/docker_build",
                  cache(**dict(ok, CMAKE_HOME_DIRECTORY="/workspace/project")))
        moved = self.tree("repo/build_moved",
                          cache(**dict(ok, CMAKE_HOME_DIRECTORY="/gone/src")))
        self.tree("repo/build_moved/inner",
                  cache(**dict(ok, CMAKE_GENERATOR="Xcode")))
        dep = self.tree(
            "repo/build_dep", cache(**dict(ok, NET_ROOT=("UNINITIALIZED", "/gone/net"))))
        gen = self.tree("repo/build_make",
                        cache(**dict(ok, CMAKE_GENERATOR="Unix Makefiles")))
        unfinished = self.tree("repo/build_unfinished",
                               cache(**dict(ok, CMAKE_PROJECT_NAME=None)))
        unfinished_make = self.tree("repo/build_unfinished_make",
                                    cache(**dict(ok, CMAKE_PROJECT_NAME=None, CMAKE_GENERATOR="Unix Makefiles")))
        buf = io.StringIO()
        with redirect_stdout(buf):
            fw.main(["stale-trees", self.tmp])
        self.assertEqual(sorted(buf.getvalue().splitlines()), sorted([
            f"{moved}\tsource gone (/gone/src)",
            f"{dep}\tNET_ROOT gone (/gone/net)",
            f"{gen}\tgenerator is 'Unix Makefiles', not Ninja",
            f"{unfinished}\tconfigure never finished",
            f"{unfinished_make}\tgenerator is 'Unix Makefiles', not Ninja",
        ]))


class Config(unittest.TestCase):
    # What the justfiles' own `compiler_suffix` / `compiler_flag` expressions gave before fw.py config replaced them
    # (`just --evaluate`, four configurations): the oracle.
    OLD = {
        ("clang", "llvm"): ("", "-DCPPLIB=libc++ -DCLIB=llvm"),
        ("clang", "newlib"): ("_newlib", "-DCPPLIB=libstdc++ -DCLIB=newlib"),
        ("gcc", "newlib"): ("_gcc", "-DCOMPILE_TARGET=arm_gcc -DCPPLIB=libstdc++ -DCLIB=newlib"),
        ("gcc", "llvm"): ("_gcc_llvm", "-DCOMPILE_TARGET=arm_gcc -DCPPLIB=libc++ -DCLIB=llvm"),
    }

    def run_config(self, compiler, libs, what):
        return subprocess.run([sys.executable, "-B", fw.__file__, "config", "--compiler", compiler, "--libs", libs,
                               what], capture_output=True, text=True)

    def test_four_configurations(self):
        for (compiler, libs), (suffix, flags) in self.OLD.items():
            with self.subTest(compiler=compiler, libs=libs):
                r = self.run_config(compiler, libs, "suffix")
                self.assertEqual(
                    (r.returncode, r.stdout, r.stderr), (0, suffix, ""))
                r = self.run_config(compiler, libs, "cmake-flags")
                self.assertEqual(
                    (r.returncode, r.stdout, r.stderr), (0, flags, ""))

    def test_bad_values_refused_for_either_output(self):
        cases = [("foo", "llvm", "error: compiler must be clang or gcc, got 'foo'\n"),
                 ("clang", "foo", "error: libs must be llvm or newlib, got 'foo'\n"),
                 ("foo", "bar", "error: compiler must be clang or gcc, got 'foo'\n")]
        for compiler, libs, text in cases:
            for what in ("suffix", "cmake-flags"):
                with self.subTest(compiler=compiler, libs=libs, what=what):
                    r = self.run_config(compiler, libs, what)
                    self.assertEqual(
                        (r.returncode, r.stdout, r.stderr), (1, "", text))

    def test_unknown_output_refused(self):
        with self.assertRaises(SystemExit):
            fw.main(["config", "--compiler", "clang",
                    "--libs", "llvm", "suffixes"])


if __name__ == "__main__":
    unittest.main()
