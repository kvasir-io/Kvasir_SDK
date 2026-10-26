#!/usr/bin/env python3
"""tools/layer_check.py on the fixture tree next to this file: the exact NEW / KEPT / STALE sets and the exit codes.

The fixture has two ranked layers (lib_low below lib_high) and a log library nested inside lib_high without a .git
of its own. It covers an upward include, a downward one, a log include from a `log = false` layer, a cross-repository
detail name next to a same-repository one and one inside a string and a comment, a nested detail namespace,
`Detail` vs `detail`, detail declarations behind a requires-clause, a `__has_include` in code and one in a comment, the
guarded `#if defined(MACRO) && __has_include` shape (exempt for a macro the rule names, also over a line continuation;
a finding for another macro or another shape), a file-level include cycle (a.hpp <-> b.hpp), two kept and a stale keep
entry. A copy in a temporary directory adds the cases that need extra files or another table (a nested checkout, a
glob that matches no file, an unknown table, malformed entries, check (d) mmio: one hit per rule, shapes it must not
match, an exemption, a stale keep, a keep's count; check (e): a cycle through a layer spelling, a kept and a stale
cycle; the overlay's `[default_repo]` taken from a list file).
No clock, no network.
"""

import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
FIXTURE = HERE / "fixture"
TOOL = HERE.parent.parent / "tools" / "layer_check.py"
SDK_TABLE = HERE.parent.parent / "tools" / "layers.toml"
STALE_LINE = '"lib_low/src/low/a.hpp -> high/gone.hpp" = "an include that is no longer there (stale)"\n'


def run(*args, root=FIXTURE, table=None):
    cmd = [sys.executable, "-B",
           str(TOOL), "--table", str(table or root / "layers.toml"), "--json", *args]
    if root is not None and "--repo" not in args:
        cmd += ["--root", str(root)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    data = json.loads(r.stdout) if r.stdout.strip().startswith("{") else None
    return r.returncode, data, r.stderr


def keys(data, label):
    return {(e["check"], e["key"]) for e in data[label]}


def keep_every_new(s):
    """The table with a keep entry for every finding of NEW."""
    for check in sorted({c for c, _ in NEW}):
        entries = "".join('"' + k.replace('"', '\\"') +
                          '" = "t"\n' for c, k in sorted(NEW) if c == check)
        header = f"[keep.{check}]\n"
        s = s.replace(header, header +
                      entries) if header in s else s + header + entries
    return s


CYCLE = "lib_high/src/high/b.hpp <-> lib_low/src/low/a.hpp"
NEW = {
    ("cycle", CYCLE),
    ("detail", "lib_high -> lib_low: low::detail::helper"),
    ("detail", "lib_high -> lib_low: low::detail::recorded"),
    ("detail", "lib_high -> lib_low: low::detail::twice"),
    ("has_include", 'lib_low/src/low/a.hpp: "high/b.hpp"'),
    ("has_include", 'lib_low/src/low/a.hpp: "high/c.hpp"'),
    ("has_include", 'lib_low/src/low/a.hpp: "logl/x.hpp"'),
    ("include", "lib_low/src/low/a.hpp -> high/b.hpp"),
    ("include", "lib_low/src/low/a.hpp -> logl/log.hpp"),
}
KEPT = {("include", "lib_low/src/low/c.hpp -> high/c.hpp"),
        ("detail", "lib_high -> lib_low: low::detail::Nested")}
STALE = {("include", "lib_low/src/low/a.hpp -> high/gone.hpp")}


# check (d): every rule once (R1 twice with the same spelling, R4 by a type pattern with a template address and a
# matched action), and what must not match - a cast without volatile, a commented-out cast, a logical && after
# readWhole, a literal address counted under R3 only, the spelling inside a string
MMIO_SOURCE = """#pragma once
inline unsigned a(unsigned x) { return *reinterpret_cast<std::uint32_t volatile*>(x); }
inline unsigned b(unsigned x) { return *reinterpret_cast<std::uint32_t volatile*>(x + 4); }
inline unsigned c(unsigned x) { return *(std::uint32_t const volatile*)x; }
inline unsigned d() { return readWhole(R::FULLREGISTER) & 3U; }
inline unsigned e() { return 3U | readWhole(R::FULLREGISTER); }
inline void f() { writeWhole(R::FULLREGISTER, readWhole(R::FULLREGISTER)); }
inline bool g() { return readWhole(R::FULLREGISTER) && ready; }
using L = FieldLocation<Register::Address<0x4000'0000, 0, 0>>;
template<unsigned Addr> using M = FieldLocation<Register::Address<Addr, 0, 0>>;
inline unsigned h(unsigned x) { return *reinterpret_cast<std::uint32_t*>(x); }
// inline unsigned i(unsigned x) { return *reinterpret_cast<std::uint64_t volatile*>(x); }
template<typename T, typename U> struct P<Register::Action<T, U>> {};
static_assert(true, "expected a Register::Action<...>");
"""
MMIO_NEW = [
    "lib_high/src/high/mmio.hpp: R1 reinterpret_cast<std::uint32_t volatile*>",
    "lib_high/src/high/mmio.hpp: R1 (std::uint32_t const volatile*)",
    "lib_high/src/high/mmio.hpp: R2 readWhole(R::FULLREGISTER) &",
    "lib_high/src/high/mmio.hpp: R2 | readWhole(",
    "lib_high/src/high/mmio.hpp: R2 writeWhole(R::FULLREGISTER, readWhole",
    "lib_high/src/high/mmio.hpp: R3 Register::Address<0x4000'0000",
    "lib_high/src/high/mmio.hpp: R4 Register::Address<",
    "lib_high/src/high/mmio.hpp: R4 Register::Action<",
]


class Copy:
    """The fixture in a temporary directory, its table edited by `edit(text) -> text`."""

    def __init__(self, edit=lambda s: s):
        self.edit = edit

    def __enter__(self) -> Path:
        self.t = tempfile.TemporaryDirectory()
        root = Path(self.t.name) / "f"
        shutil.copytree(FIXTURE, root)
        table = root / "layers.toml"
        table.write_text(self.edit(table.read_text()))
        return root

    def __exit__(self, *exc):
        self.t.cleanup()


class LayerCheck(unittest.TestCase):
    def test_sets_and_report_mode(self):
        rc, data, err = run("--report")
        self.assertEqual(rc, 0, err)
        self.assertEqual(keys(data, "new"), NEW)
        self.assertEqual(keys(data, "kept"), KEPT)
        self.assertEqual(keys(data, "stale"), STALE)
        self.assertEqual(data["scanned"], {
                         "lib_low": 2, "lib_high": 2, "lib_log": 1})
        self.assertEqual(data["cycles"], [["high", "low"]])
        where = {e["key"]: e["where"] for e in data["new"]}
        self.assertEqual(
            where["lib_low/src/low/a.hpp -> high/b.hpp"], ["lib_low/src/low/a.hpp:3"])
        self.assertEqual(
            where["lib_high -> lib_low: low::detail::helper"], ["lib_high/src/high/b.hpp:9"])
        self.assertEqual(
            where["lib_high -> lib_low: low::detail::recorded"], ["lib_high/src/high/b.hpp:15"])
        self.assertEqual(
            where[CYCLE], ["lib_high/src/high/b.hpp:2", "lib_low/src/low/a.hpp:3"])
        # the two guarded lines are exempt; the OTHER-macro line is the only "high/c.hpp" one
        self.assertEqual(
            where['lib_low/src/low/a.hpp: "high/c.hpp"'], ["lib_low/src/low/a.hpp:32"])

    def test_fails_on_new_and_stale(self):
        rc, data, err = run()
        self.assertEqual(rc, 1, err)
        self.assertEqual(keys(data, "new"), NEW)

    def test_stale_alone_fails(self):
        with Copy(keep_every_new) as root:
            rc, data, err = run(root=root)
            self.assertEqual(rc, 1, err)
            self.assertEqual(
                (keys(data, "new"), keys(data, "stale")), (set(), STALE))

    def test_passes_when_every_finding_is_kept(self):
        with Copy(lambda s: keep_every_new(s.replace(STALE_LINE, ""))) as root:
            rc, data, err = run(root=root)
            self.assertEqual(rc, 0, err)
            self.assertEqual(
                (keys(data, "new"), keys(data, "stale")), (set(), set()))
            self.assertEqual(len(data["kept"]), len(NEW) + 2)

    def test_guard_rule_names_the_macro(self):
        # without the rule the guarded lines are findings too
        with Copy(lambda s: s.replace('guarded_has_include = ["LOW_LOG"]', "guarded_has_include = []")) as root:
            _, data, _ = run("--report", root=root)
            self.assertEqual(keys(
                data, "new") - NEW, {("has_include", 'lib_low/src/low/a.hpp: <logl/log.hpp>')})
            where = {e["key"]: e["where"] for e in data["new"]}
            self.assertEqual(
                len(where['lib_low/src/low/a.hpp: "high/c.hpp"']), 2)

    def test_a_repository_not_scanned_is_neither_checked_nor_stale(self):
        rc, data, err = run(
            "--repo", f"lib_high={FIXTURE / 'lib_high'}", root=None, table=FIXTURE / "layers.toml")
        self.assertEqual(rc, 0, err)
        self.assertEqual(data["scanned"], {"lib_high": 2, "lib_log": 1})
        self.assertEqual((data["new"], data["stale"]), ([], []))

    def test_nested_checkout_is_not_walked(self):
        with Copy() as root:
            vend = root / "lib_low" / "src" / "low" / "vendored"
            vend.mkdir()
            (vend / ".git").write_text("gitdir: elsewhere\n")
            (vend / "v.hpp").write_text('#include "high/c.hpp"\n')
            rc, data, err = run("--report", root=root)
            self.assertEqual(rc, 0, err)
            self.assertEqual(keys(data, "new"), NEW)
            self.assertEqual(data["scanned"]["lib_low"], 2)

    def test_an_unknown_table_is_a_table_error(self):
        for table in ("allow", "compat", "kep"):
            entry = f'[{table}.include]\n"lib_low/src/low/a.hpp -> high/b.hpp" = "t"\n'
            with self.subTest(table), Copy(lambda s, e=entry: s + e) as root:
                for args in ((), ("--report",)):
                    rc, _, err = run(*args, root=root)
                    self.assertEqual(rc, 2)
                    self.assertIn(f"unknown table(s) [{table}]", err)

    def test_table_errors_exit_2(self):
        bad = {
            "needs a reason": lambda s: s.replace('"kept upward include"', '" "'),
            "the checks are": lambda s: s + '[keep.includes]\n"x" = "t"\n',
            "rules: unknown key": lambda s: s.replace("[rules]\n", '[rules]\nguarded = ["X"]\n'),
            "a list of macro names": lambda s: s.replace('["LOW_LOG"]', '["LOW LOG"]'),
        }
        for message, edit in bad.items():
            with self.subTest(message), Copy(edit) as root:
                rc, _, err = run(root=root)
                self.assertEqual(rc, 2, err)
                self.assertIn(message, err)
        with Copy() as root:
            overlay = root.parent / "glob.toml"
            overlay.write_text('[repo.lib_low.paths]\n"nothing/**" = "low"\n')
            rc, data, _ = run("--overlay", str(overlay), root=root)
            self.assertEqual(rc, 2)
            self.assertEqual(data["errors"], [
                             "repo lib_low: path glob 'nothing/**' matches no file"])
            overlay.write_text('[repo.lib_low.paths]\n"src/**" = "nowhere"\n')
            rc, _, err = run("--overlay", str(overlay), root=root)
            self.assertEqual(rc, 2)
            self.assertIn("unknown layer 'nowhere'", err)

    def test_mmio_rules(self):
        with Copy() as root:
            (root / "lib_high" / "src" / "high" /
             "mmio.hpp").write_text(MMIO_SOURCE)
            rc, data, err = run("--report", root=root)
            self.assertEqual(rc, 0, err)
            self.assertEqual({k for c, k in keys(data, "new")
                             if c == "mmio"}, set(MMIO_NEW))
            where = {e["key"]: e["where"] for e in data["new"]}
            self.assertEqual(where["lib_high/src/high/mmio.hpp: R3 Register::Address<0x4000'0000"],
                             ["lib_high/src/high/mmio.hpp:9"])
            self.assertEqual(len(
                where["lib_high/src/high/mmio.hpp: R1 reinterpret_cast<std::uint32_t volatile*>"]), 2)
            self.assertEqual(
                where["lib_high/src/high/mmio.hpp: R4 Register::Address<"], ["lib_high/src/high/mmio.hpp:10"])
            self.assertEqual(
                where["lib_high/src/high/mmio.hpp: R4 Register::Action<"], ["lib_high/src/high/mmio.hpp:13"])

    def test_mmio_exempt_keep_count_and_stale(self):
        hits = MMIO_NEW
        twice = "lib_high/src/high/mmio.hpp: R1 reinterpret_cast<std::uint32_t volatile*>"
        cases = {
            # every finding listed: a count of 2 for the key that matches twice
            "all kept": ("".join(f'"{k}" = ' + ('["t", 2]' if k == twice else '"t"') + "\n" for k in hits),
                         set(), set()),
            # the count too low: NEW again
            "count raised": ("".join(f'"{k}" = "t"\n' for k in hits), {twice}, set()),
            # the count too high, and an entry whose finding is gone: STALE
            "stale": ("".join(f'"{k}" = ' + ('["t", 3]' if k == twice else '"t"') + "\n" for k in hits)
                      + '"lib_high/src/high/mmio.hpp: R1 static_cast<int volatile*>" = "gone"\n',
                      set(), {twice, "lib_high/src/high/mmio.hpp: R1 static_cast<int volatile*>"}),
        }
        for name, (entries, new, stale) in cases.items():
            with self.subTest(name), Copy(lambda s: s + "[keep.mmio]\n" + entries) as root:
                (root / "lib_high" / "src" / "high" /
                 "mmio.hpp").write_text(MMIO_SOURCE)
                rc, data, err = run(root=root)
                self.assertEqual(
                    {k for c, k in keys(data, "new") if c == "mmio"}, new, name)
                self.assertEqual(
                    {k for c, k in keys(data, "stale") if c == "mmio"}, stale, name)
        exempt = 'mmio_exempt = { R1 = ["lib_high/src/high/mm*.hpp"] }\n'
        with Copy(lambda s: s.replace("[rules]\n", "[rules]\n" + exempt)) as root:
            (root / "lib_high" / "src" / "high" /
             "mmio.hpp").write_text(MMIO_SOURCE)
            _, data, _ = run("--report", root=root)
            self.assertEqual({k for c, k in keys(data, "new") if c == "mmio"},
                             {k for k in hits if ": R1 " not in k})

    def test_mmio_table_errors(self):
        bad = {
            "count >= 2": lambda s: s + '[keep.mmio]\n"x: R1 y" = ["t", 1]\n',
            "rules.mmio_exempt": lambda s: s.replace("[rules]\n", '[rules]\nmmio_exempt = { R9 = ["x"] }\n'),
        }
        for message, edit in bad.items():
            with self.subTest(message), Copy(edit) as root:
                rc, _, err = run(root=root)
                self.assertEqual(rc, 2, err)
                self.assertIn(message, err)

    def test_cycles(self):
        # through a layer spelling (every file of the layer whose path ends in it), a kept one, a stale one
        spelling = '[[spelling]]\nprefix = "tbl/"\nlayer = "high"\n'
        kept = f'[keep.cycle]\n"{CYCLE}" = "t"\n"lib_high/src/high/c.hpp <-> lib_low/src/low/c.hpp" = "gone"\n'
        with Copy(lambda s: s.replace("[rules]\n", spelling + "[rules]\n") + kept) as root:
            (root / "lib_low" / "src" / "low" /
             "front.hpp").write_text('#pragma once\n#include "tbl/table.hpp"\n')
            (root / "lib_high" / "src" / "tbl").mkdir()
            (root / "lib_high" / "src" / "tbl" /
             "table.hpp").write_text('#pragma once\n#include "low/front.hpp"\n')
            _, data, err = run("--report", root=root)
            self.assertEqual({k for c, k in keys(data, "new") if c == "cycle"},
                             {"lib_high/src/tbl/table.hpp <-> lib_low/src/low/front.hpp"}, err)
            self.assertEqual(
                {k for c, k in keys(data, "kept") if c == "cycle"}, {CYCLE})
            self.assertEqual({k for c, k in keys(data, "stale") if c == "cycle"},
                             {"lib_high/src/high/c.hpp <-> lib_low/src/low/c.hpp"})

    def test_default_repos_from_a_list(self):
        # the checkouts a list file names, in layer `app`; a folder it does not name is not scanned
        with Copy() as root:
            for name in ("fw_a", "fw_old"):
                (root / name).mkdir()
                (root / name / "main.cpp").write_text(
                    '#include "low/a.hpp"\nint m = low::detail::helper(1);\n')
            (root / "repos.txt").write_text("# checkouts\nnolink lib_low\nfw_a  -  master\n")
            overlay = root.parent / "ws.toml"
            overlay.write_text(
                '[default_repo]\nlayer = "app"\nlist = "repos.txt"\n')
            rc, data, err = run("--report", "--overlay",
                                str(overlay), root=root)
            self.assertEqual(rc, 0, err)
            self.assertIn("fw_a", data["scanned"])
            self.assertNotIn("fw_old", data["scanned"])
            self.assertIn(
                ("detail", "fw_a -> lib_low: low::detail::helper"), keys(data, "new"))
            overlay.write_text(
                '[default_repo]\nlayer = "app"\nrequire = "CMakeLists.txt"\n')
            rc, _, err = run("--overlay", str(overlay), root=root)
            self.assertEqual(rc, 2)
            self.assertIn("default_repo.require: unknown key", err)

    def test_sdk_table_loads(self):
        # the real table parses, has only tables the tool knows, and every layer and spelling it names exists (nothing
        # scanned)
        rc, data, err = run(root=None, table=SDK_TABLE)
        self.assertEqual(rc, 0, err)
        self.assertEqual(data["scanned"], {})


if __name__ == "__main__":
    unittest.main(verbosity=2)
